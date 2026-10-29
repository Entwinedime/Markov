#include <exe_graph/runtime/tensor.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class MatrixSelector {
public:
    explicit MatrixSelector(const char * path) : library_(dlopen(path, RTLD_NOW | RTLD_LOCAL), dlclose) {
        if (!library_) throw std::runtime_error(dlerror());
        check_ = reinterpret_cast<Check>(dlsym(library_.get(), "LegacyMmCheckHitV3Shape"));
        if (check_ == nullptr) throw std::runtime_error(dlerror());
    }

    bool use_v3(std::int64_t rows, std::int64_t inner, std::int64_t columns, ge::DataType dtype, bool with_bias, std::uint32_t cores,
                const std::string & soc) const {
        gert::Tensor left;
        gert::Tensor right;
        gert::Tensor bias;
        left.MutableOriginShape() = gert::Shape({ rows, inner });
        right.MutableOriginShape() = gert::Shape({ columns, inner });
        bias.MutableOriginShape() = gert::Shape({ columns });
        left.MutableStorageShape() = left.GetOriginShape();
        right.MutableStorageShape() = right.GetOriginShape();
        bias.MutableStorageShape() = bias.GetOriginShape();

        // Match the installed bridge's metadata conversion; no device data is allocated.
        for (auto * tensor : { &left, &right, &bias }) {
            tensor->SetOriginFormat(ge::FORMAT_ND);
            tensor->SetStorageFormat(ge::FORMAT_ND);
            tensor->SetDataType(dtype);
        }

        return check_(&left, &right, with_bias ? &bias : nullptr, false, true, ge::FORMAT_ND, false, cores, soc);
    }

private:
    using Check = bool (*)(const gert::Tensor *, const gert::Tensor *, const gert::Tensor *, bool, bool, ge::Format, bool, std::uint32_t, const std::string &);
    std::unique_ptr<void, int (*)(void *)> library_;
    Check check_;
};

std::int64_t positive_integer(std::string_view text) {
    std::int64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0) throw std::invalid_argument("positive integer required");
    return value;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc != 9) throw std::invalid_argument("library, cube cores, SoC, K, N, dtype, maximum rows and bias mode required");
        const auto cores = positive_integer(argv[2]);
        if (cores > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("cube core count is too large");
        const auto inner = positive_integer(argv[4]);
        const auto columns = positive_integer(argv[5]);
        const auto maximum = positive_integer(argv[7]);
        const std::string_view type(argv[6]);
        if (type != "torch.bfloat16" && type != "torch.float16") throw std::invalid_argument("BF16 or FP16 required");
        const std::string_view bias(argv[8]);
        if (bias != "bias" && bias != "none") throw std::invalid_argument("bias or none required");

        const MatrixSelector selector(argv[1]);
        const std::string soc(argv[3]);
        const auto dtype = type == "torch.bfloat16" ? ge::DT_BF16 : ge::DT_FLOAT16;
        std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
        std::optional<std::int64_t> first;

        // Exhaust the declared integer domain, then compress without fitting a threshold.
        for (std::int64_t row = 1;; ++row) {
            if (selector.use_v3(row, inner, columns, dtype, bias == "bias", static_cast<std::uint32_t>(cores), soc)) {
                if (!first) first = row;
            }
            else if (first) {
                intervals.emplace_back(*first, row - 1);
                first.reset();
            }
            if (row == maximum) break;
        }
        if (first) intervals.emplace_back(*first, maximum);
        std::printf("{\"v3_intervals\":[");
        for (std::size_t index = 0; index < intervals.size(); ++index) {
            if (index) std::printf(",");
            std::printf("[%lld,%lld]", static_cast<long long>(intervals[index].first), static_cast<long long>(intervals[index].second));
        }
        std::printf("]}\n");
    }
    catch (const std::exception & error) {
        std::fprintf(stderr, "matrix selection: %s\n", error.what());
        return 1;
    }
}
