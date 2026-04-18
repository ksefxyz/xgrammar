import xgrammar.grammar as grammar_module


def test_convert_schema_to_str_uses_raw_pydantic_v1_schema_json(monkeypatch):
    original_base_model = grammar_module.BaseModel

    class _FakeBaseModel:
        pass

    class _FakePydanticV1Model(_FakeBaseModel):
        @classmethod
        def schema_json(cls):
            return '{"type":"object","properties":{"x":{"type":"integer"}}}'

    monkeypatch.setattr(grammar_module, "BaseModel", _FakeBaseModel)
    try:
        schema_str = grammar_module._convert_schema_to_str(_FakePydanticV1Model)
    finally:
        monkeypatch.setattr(grammar_module, "BaseModel", original_base_model)

    assert schema_str == '{"type":"object","properties":{"x":{"type":"integer"}}}'
