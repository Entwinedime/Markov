"""Token probe optimizations must preserve existing identities and dictionaries."""

import hashlib
import struct
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch
from types import SimpleNamespace

from trace_sim_probe.probes.hicache import tokens


def previous_identity(values):
    digest = hashlib.sha256()
    for value in values:
        for item in value if isinstance(value, (list, tuple)) else (value,):
            digest.update(int(item).to_bytes(4, "little", signed=False))
    return "sha256_u32le:" + digest.hexdigest()


class TokenFieldsCheck(unittest.TestCase):
    def test_request_fields_reuse_one_event_snapshot(self):
        req = SimpleNamespace(origin_input_ids=[1, 2, 3], output_ids=[4], kv_committed_len=3)
        bound = {"req": req}
        with (
            patch.object(tokens, "_request_tokens", wraps=tokens._request_tokens) as read,
            patch.object(tokens, "_token_path_id", wraps=tokens._token_path_id) as digest,
        ):
            _, span = tokens._extract_request_token_span("arg:req,committed", bound, (), None)
            _, path = tokens._extract_request_token_path("arg:req,committed", bound, (), None)
            _, values = tokens._extract_request_tokens("arg:req,committed", bound, (), None)
            self.assertEqual((read.call_count, digest.call_count), (1, 1))
        self.assertEqual(values, [1, 2, 3])
        self.assertEqual(path["token_ids"], values)
        self.assertEqual(span["path_id"], previous_identity(values))
        self.assertEqual(path["token_path_id"], span["path_id"])
        _, output = tokens._extract_request_tokens("arg:req,output", bound, (), None)
        self.assertEqual(output, [4])
        req.kv_committed_len = 4
        _, changed = tokens._extract_request_token_span("arg:req,committed", {"req": req}, (), None)
        self.assertEqual(changed["token_count"], 4)
        self.assertNotEqual(changed["path_id"], span["path_id"])

    def test_request_count_does_not_force_hashing(self):
        req = SimpleNamespace(fill_ids=[7, 8])
        with patch.object(tokens, "_token_path_id", side_effect=AssertionError("unexpected hash")):
            found, values = tokens._extract_request_tokens("arg:req,fill", {"req": req}, (), None)
        self.assertTrue(found)
        self.assertEqual(values, [7, 8])

    def setUp(self):
        reset = patch.dict(tokens._TOKEN_PATHS_EMITTED_BY_SCOPE, clear=True)
        reset.start()
        self.addCleanup(reset.stop)

    def test_identity_is_byte_identical(self):
        for values in ([], [0, 1, 2**32 - 1], [(1, 2), [3, 4], 5], list(range(2944))):
            with self.subTest(values=values[:3]):
                self.assertEqual(tokens._token_path_id(values), previous_identity(values))
        for values in ([-1], [2**32], [(1, -1)]):
            with self.assertRaises((OverflowError, struct.error)):
                tokens._token_path_id(values)

    def test_path_and_span_share_only_one_event_observation(self):
        values = [1, 2, 3]
        bound = {"token_ids": values}
        with (
            patch.object(tokens, "_tokens_for_path", wraps=tokens._tokens_for_path) as normalize,
            patch.object(tokens, "_token_path_id", wraps=tokens._token_path_id) as identity,
        ):
            _, path = tokens._extract_token_path("arg:token_ids", bound, (), None)
            _, span = tokens._extract_token_span("arg:token_ids", bound, (), None)
            self.assertEqual((normalize.call_count, identity.call_count), (1, 1))
        self.assertEqual(path["token_ids"], values)
        self.assertEqual(path["token_path_id"], span["path_id"])
        values.append(4)
        _, changed = tokens._extract_token_span("arg:token_ids", {"token_ids": values}, (), None)
        self.assertEqual(changed["token_count"], 4)
        self.assertNotEqual(changed["path_id"], span["path_id"])

    def test_span_first_and_dictionary_buckets(self):
        values = [(1, 2), (3, 4)]
        for consumer in ([], ["hicache_state_model"]):
            bound = {"token_ids": values, "__trace_sim_fact_consumers": consumer, "__trace_sim_phase": "end"}
            _, span = tokens._extract_token_span("arg:token_ids", bound, (), None)
            _, path = tokens._extract_token_path("arg:token_ids", bound, (), None)
            self.assertEqual(path["token_ids"], [[1, 2], [3, 4]])
            self.assertEqual(span["path_id"], path["token_path_id"])
            _, repeated = tokens._extract_token_path("arg:token_ids", dict(bound), (), None)
            self.assertNotIn("token_ids", repeated)

    def test_independent_contexts_do_not_share_observations(self):
        def observe(value):
            values = [value, value + 1]
            bound = {"token_ids": values}
            _, path = tokens._extract_token_path("arg:token_ids", bound, (), None)
            _, span = tokens._extract_token_span("arg:token_ids", bound, (), None)
            return path["token_path_id"], span["path_id"], previous_identity(values)

        with ThreadPoolExecutor(max_workers=4) as pool:
            for path, span, expected in pool.map(observe, range(32)):
                self.assertEqual(path, expected)
                self.assertEqual(span, expected)


if __name__ == "__main__":
    unittest.main()
