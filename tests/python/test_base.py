import xgrammar as xgr


def test_xgr_object_is_hashable():
    grammar = xgr.Grammar.from_ebnf('root ::= "a"')

    values = {grammar}

    assert grammar in values
