import xgrammar.structural_tag as structural_tag
from xgrammar.structural_tag import ConstStringFormat, OrFormat, SequenceFormat, StructuralTag


def test_or_format_forward_refs_are_resolved():
    structural_tag_obj = StructuralTag(
        format=OrFormat(
            elements=[
                ConstStringFormat(value="A"),
                SequenceFormat(elements=[ConstStringFormat(value="B")]),
            ]
        )
    )

    assert structural_tag_obj.format.type == "or"
    assert structural_tag_obj.format.elements[1].type == "sequence"


def test_dispatch_formats_are_exported():
    assert "DispatchFormat" in structural_tag.__all__
    assert "TokenDispatchFormat" in structural_tag.__all__
