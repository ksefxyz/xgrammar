import importlib
import sys
import types

import pytest
import torch

import xgrammar.contrib.hf as hf_contrib


class _FakeTokenizerInfo:
    vocab_size = 4


class _FakeCompiledGrammar:
    tokenizer_info = _FakeTokenizerInfo()


class _FakeMatcher:
    def __init__(self, accept_result=True):
        self.accept_result = accept_result
        self.accepted_tokens = []
        self.fill_calls = 0

    def is_terminated(self):
        return False

    def accept_token(self, token):
        self.accepted_tokens.append(int(token))
        return self.accept_result

    def fill_next_token_bitmask(self, bitmask, index=0):
        self.fill_calls += 1


def test_hf_logits_processor_raises_when_sampled_token_is_rejected(monkeypatch):
    fake_matcher = _FakeMatcher(accept_result=False)

    monkeypatch.setattr(
        hf_contrib.xgr, "GrammarMatcher", lambda compiled_grammar: fake_matcher
    )
    monkeypatch.setattr(
        hf_contrib.xgr,
        "allocate_token_bitmask",
        lambda batch_size, vocab_size: torch.zeros((batch_size, 1), dtype=torch.int32),
    )
    monkeypatch.setattr(hf_contrib.xgr, "apply_token_bitmask_inplace", lambda logits, bitmask: None)

    processor = hf_contrib.LogitsProcessor(_FakeCompiledGrammar())
    scores = torch.zeros((1, 4), dtype=torch.float32)

    processor(torch.tensor([[1]], dtype=torch.long), scores.clone())

    with pytest.raises(RuntimeError, match="Sampled token 2 is rejected by grammar matcher 0"):
        processor(torch.tensor([[1, 2]], dtype=torch.long), scores.clone())

    assert fake_matcher.accepted_tokens == [2]


def test_mlxlm_contrib_imports_with_mlx_kernel_symbol(monkeypatch):
    fake_mx_core = types.ModuleType("mlx.core")
    fake_mx_core.array = lambda value, dtype=None: value
    fake_mx_core.compile = lambda fn: fn
    fake_mx_core.uint8 = object()
    fake_mx_core.int32 = object()

    fake_mlx_pkg = types.ModuleType("mlx")
    fake_mlx_pkg.core = fake_mx_core

    fake_generate_module = types.ModuleType("mlx_lm.generate")
    fake_generate_module.generate = lambda *args, **kwargs: None

    fake_utils_module = types.ModuleType("mlx_lm.utils")
    fake_utils_module.load = lambda *args, **kwargs: (None, None)

    fake_mlx_lm_pkg = types.ModuleType("mlx_lm")
    fake_mlx_lm_pkg.generate = fake_generate_module
    fake_mlx_lm_pkg.utils = fake_utils_module

    monkeypatch.setitem(sys.modules, "mlx", fake_mlx_pkg)
    monkeypatch.setitem(sys.modules, "mlx.core", fake_mx_core)
    monkeypatch.setitem(sys.modules, "mlx_lm", fake_mlx_lm_pkg)
    monkeypatch.setitem(sys.modules, "mlx_lm.generate", fake_generate_module)
    monkeypatch.setitem(sys.modules, "mlx_lm.utils", fake_utils_module)

    sys.modules.pop("xgrammar.kernels.apply_token_bitmask_mlx", None)
    sys.modules.pop("xgrammar.contrib.mlxlm", None)

    module = importlib.import_module("xgrammar.contrib.mlxlm")

    assert module.apply_token_bitmask_mlx is not None
    assert hasattr(module, "XGrammarLogitsProcessor")


def test_hf_logits_processor_keeps_full_bitmask_for_terminated_matchers(monkeypatch):
    active_matcher = _FakeMatcher(accept_result=True)

    class _TerminatedMatcher(_FakeMatcher):
        def is_terminated(self):
            return True

    terminated_matcher = _TerminatedMatcher()
    created = []

    def make_matcher(_compiled_grammar):
        matcher = [active_matcher, terminated_matcher][len(created)]
        created.append(matcher)
        return matcher

    monkeypatch.setattr(hf_contrib.xgr, "GrammarMatcher", make_matcher)
    monkeypatch.setattr(
        hf_contrib.xgr,
        "allocate_token_bitmask",
        lambda batch_size, vocab_size: torch.ones((batch_size, 1), dtype=torch.int32),
    )
    monkeypatch.setattr(hf_contrib.xgr, "apply_token_bitmask_inplace", lambda logits, bitmask: None)

    processor = hf_contrib.LogitsProcessor([_FakeCompiledGrammar(), _FakeCompiledGrammar()])
    scores = torch.zeros((2, 4), dtype=torch.float32)
    input_ids = torch.tensor([[1], [1]], dtype=torch.long)

    processor(input_ids, scores)

    assert processor.token_bitmask[0, 0].item() == 1
    assert processor.token_bitmask[1, 0].item() == -1
