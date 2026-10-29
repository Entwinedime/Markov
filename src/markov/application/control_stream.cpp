#include "markov/application/control_stream.hpp"

#include <stdexcept>
#include <string>

namespace markov {

ControlStream::ControlStream(std::istream & input, std::ostream & output) : input_(input), output_(output) {}

nlohmann::json ControlStream::receive() {
    std::string line;
    if (!std::getline(input_, line)) throw std::runtime_error("native control disconnected before completing its work");
    return nlohmann::json::parse(line);
}

void ControlStream::send(const nlohmann::json & message) {
    output_ << message.dump() << '\n' << std::flush;
    if (!output_) throw std::runtime_error("cannot deliver native control feedback");
}

} // namespace markov
