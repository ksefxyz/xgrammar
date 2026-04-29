import copy
import json
from functools import lru_cache
from pathlib import Path

import pytest

import xgrammar as xgr
from xgrammar.testing import _is_grammar_accept_string


def _seller_party():
    return {
        "DaneIdentyfikacyjne": {"Nazwa": "Seller", "NIP": "1234567890"},
        "Adres": {"AdresL1": "ul. Seller 1"},
    }


def _buyer_nip(with_address=True):
    result = {"DaneIdentyfikacyjne": {"Nazwa": "Buyer", "NIP": "9876543210"}}
    if with_address:
        result["Adres"] = {"AdresL1": "ul. Buyer 1"}
    return result


def _buyer_ue_vat():
    return {
        "DaneIdentyfikacyjne": {
            "Nazwa": "Buyer",
            "KodUE": "DE",
            "NrVatUE": "DE123456789",
        },
        "Adres": {"AdresL1": "ul. Buyer 1"},
    }


def _buyer_nrid():
    return {
        "DaneIdentyfikacyjne": {"Nazwa": "Buyer", "NrID": "ABC123"},
        "Adres": {"AdresL1": "ul. Buyer 1"},
    }


def _buyer_brakid():
    return {
        "DaneIdentyfikacyjne": {"Nazwa": "Buyer", "BrakID": 1},
        "Adres": {"AdresL1": "ul. Buyer 1"},
    }


def _vat_row(*, include_p9a=False, include_p11=False):
    row = {"P_7": "Usługa", "P_8B": 1, "P_12": "23"}
    if include_p9a:
        row["P_9A"] = 100
    if include_p11:
        row["P_11"] = 100
    return row


def _payment_cash():
    return {"FormaPlatnosci": 1}


def _payment_transfer_with_account():
    return {
        "FormaPlatnosci": 6,
        "RachunekBankowy": [
            {"NrRB": "PL61109010140000071219812874", "NazwaBanku": "PKO BP"}
        ],
    }


CASES = {
    "payment_non_transfer_without_account_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "payment_transfer_without_account_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": {"FormaPlatnosci": 6},
            },
        },
    },
    "payment_transfer_with_account_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_transfer_with_account(),
            },
        },
    },
    "vat_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "vat_missing_fawiersz_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {"RodzajFaktury": "VAT", "P_2": "FV/1", "Platnosc": _payment_cash()},
        },
    },
    "vat_with_zamowienie_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "non_upr_missing_podmiot2_adresl1_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": {"DaneIdentyfikacyjne": {"Nazwa": "Buyer", "NIP": "9876543210"}},
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "upr_valid_with_p15_only": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "UPR",
                "P_2": "FV/1",
                "P_15": 100,
                "Platnosc": _payment_cash(),
            },
        },
    },
    "upr_valid_with_fawiersz_only": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "UPR",
                "P_2": "FV/1",
                "P_15": 100,
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "upr_missing_p15_and_fawiersz_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {"RodzajFaktury": "UPR", "P_2": "FV/1", "Platnosc": _payment_cash()},
        },
    },
    "upr_with_zamowienie_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "UPR",
                "P_2": "FV/1",
                "P_15": 100,
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "zal_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "ZAL",
                "P_2": "FV/1",
                "P_15": 100,
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "zal_missing_p15_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "ZAL",
                "P_2": "FV/1",
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "zal_missing_zamowienie_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "ZAL",
                "P_2": "FV/1",
                "P_15": 100,
                "Platnosc": _payment_cash(),
            },
        },
    },
    "roz_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "ROZ",
                "P_2": "FV/1",
                "P_15": 100,
                "FakturaZaliczkowa": [{"NrFaZaliczkowej": "FZ/1"}],
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "roz_missing_fakturazaliczkowa_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "ROZ",
                "P_2": "FV/1",
                "P_15": 100,
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "roz_with_zamowienie_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "ROZ",
                "P_2": "FV/1",
                "P_15": 100,
                "FakturaZaliczkowa": [{"NrFaZaliczkowej": "FZ/1"}],
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_missing_danefakorygowanej_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_with_zamowienie_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_zal_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR_ZAL",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "P_15": 100,
                "P_15ZK": 50,
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_zal_missing_p15zk_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR_ZAL",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "P_15": 100,
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_zal_with_fakturazaliczkowa_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR_ZAL",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "P_15": 100,
                "P_15ZK": 50,
                "Zamowienie": {"WartoscZamowienia": 100},
                "FakturaZaliczkowa": [{"NrFaZaliczkowej": "FZ/1"}],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_roz_valid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR_ROZ",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "P_15ZK": 50,
                "FakturaZaliczkowa": [{"NrFaZaliczkowej": "FZ/1"}],
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_roz_missing_fakturazaliczkowa_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR_ROZ",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "P_15ZK": 50,
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "kor_roz_with_zamowienie_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "KOR_ROZ",
                "P_2": "FV/1",
                "PrzyczynaKorekty": "błąd",
                "TypKorekty": 1,
                "DaneFaKorygowanej": [
                    {"NrFaKorygowanej": "FV/OLD/1", "DataWystFaKorygowanej": "2026-04-14"}
                ],
                "P_15ZK": 50,
                "FakturaZaliczkowa": [{"NrFaZaliczkowej": "FZ/1"}],
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Zamowienie": {"WartoscZamowienia": 100},
                "Platnosc": _payment_cash(),
            },
        },
    },
    "podmiot2_identity_valid_nip": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "podmiot2_identity_valid_ue_vat": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_ue_vat(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "podmiot2_identity_valid_nrid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nrid(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "podmiot2_identity_valid_brakid": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_brakid(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "podmiot2_identity_missing_identity_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": {
                "DaneIdentyfikacyjne": {"Nazwa": "Buyer"},
                "Adres": {"AdresL1": "ul. Buyer 1"},
            },
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "fawiersz_valid_with_p9a": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p9a=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "fawiersz_valid_with_p11": {
        "expected": "VALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row(include_p11=True)],
                "Platnosc": _payment_cash(),
            },
        },
    },
    "fawiersz_missing_p9a_and_p11_invalid": {
        "expected": "INVALID",
        "payload": {
            "Podmiot1": _seller_party(),
            "Podmiot2": _buyer_nip(),
            "Fa": {
                "RodzajFaktury": "VAT",
                "P_2": "FV/1",
                "FaWiersz": [_vat_row()],
                "Platnosc": _payment_cash(),
            },
        },
    },
}


CASE_KEYS = {
    "DaneFaKorygowanej",
    "FakturaZaliczkowa",
    "FaWiersz",
    "FP",
    "KursWalutyZ",
    "KursWalutyZK",
    "NrFaKorygowany",
    "OkresFaKorygowanej",
    "P_15",
    "P_15ZK",
    "PrzyczynaKorekty",
    "RodzajFaktury",
    "TP",
    "TypKorekty",
    "ZaliczkaCzesciowa",
    "Zamowienie",
    "ZwrotAkcyzy",
}
SALE_PERIOD_KEYS = {"OkresFa", "P_6"}


def _deep_merge(base, override):
    if isinstance(base, dict) and isinstance(override, dict):
        result = copy.deepcopy(base)
        for key, value in override.items():
            if key in result:
                result[key] = _deep_merge(result[key], value)
            else:
                result[key] = copy.deepcopy(value)
        return result
    if isinstance(base, list) and isinstance(override, list):
        return copy.deepcopy(override)
    return copy.deepcopy(override)


def _baseline_payload():
    return {
        "Naglowek": {
            "KodFormularza": {"value": "FA"},
            "WariantFormularza": 3,
            "DataWytworzeniaFa": "2026-04-14T10:00:00Z",
        },
        "Fa": {
            "KodWaluty": "PLN",
            "P_1": "2026-04-14",
            "Adnotacje": {
                "P_16": 1,
                "P_17": 1,
                "P_18": 1,
                "P_18A": 1,
                "P_23": 1,
                "PMarzy": {"P_PMarzyN": 1},
                "Zwolnienie": {"P_19N": 1},
                "NoweSrodkiTransportu": {"P_22N": 1},
            },
        },
        "_x_podmiot2_case": {"NONE": {}},
    }


def _ensure_address_defaults(node):
    if isinstance(node, dict):
        if "AdresL1" in node and "KodKraju" not in node:
            node["KodKraju"] = "PL"
        for value in node.values():
            _ensure_address_defaults(value)
    elif isinstance(node, list):
        for item in node:
            _ensure_address_defaults(item)


def _move_matching_keys(source, target, predicate):
    for key in list(source.keys()):
        if predicate(key):
            target[key] = source.pop(key)


def _normalize_fa_structure(root):
    fa = root.get("Fa")
    if not isinstance(fa, dict):
        return

    sale_period_block = copy.deepcopy(fa.get("_x_sale_period", {}))
    tax_totals_block = copy.deepcopy(fa.get("_x_tax_totals", {}))

    _move_matching_keys(fa, sale_period_block, lambda key: key in SALE_PERIOD_KEYS)
    _move_matching_keys(fa, tax_totals_block, lambda key: key.startswith("P_13_") or key.startswith("P_14_"))

    if sale_period_block:
        fa["_x_sale_period"] = sale_period_block
    if tax_totals_block:
        fa["_x_tax_totals"] = tax_totals_block

    for index, item in enumerate(fa.get("FaWiersz", []), start=1):
        item.setdefault("NrWierszaFa", index)

    zamowienie = fa.get("Zamowienie")
    if isinstance(zamowienie, dict) and "WartoscZamowienia" in zamowienie:
        zamowienie.setdefault("ZamowienieWiersz", [{"NrWierszaZam": 1}])

    for item in fa.get("DaneFaKorygowanej", []):
        if "NrKSeF" not in item and "NrKSeFN" not in item:
            item["NrKSeFN"] = 1

    for item in fa.get("FakturaZaliczkowa", []):
        if "NrKSeFFaZaliczkowej" not in item and "NrKSeFZN" not in item:
            item["NrKSeFZN"] = 1


def _normalize_podmiot2_case(root):
    if "_x_podmiot2_case" in root:
        return

    podmiot2 = root.get("Podmiot2")
    if not isinstance(podmiot2, dict):
        root["_x_podmiot2_case"] = {"NONE": {}}
        return

    jst = podmiot2.pop("JST", None) == 1
    gv = podmiot2.pop("GV", None) == 1
    if jst and gv:
        root["_x_podmiot2_case"] = {
            "JST_GV": {
                "_x_podmiot3_jst_role": {
                    "DaneIdentyfikacyjne": {"BrakID": 1},
                    "Rola": 8,
                },
                "_x_podmiot3_gv_role": {
                    "DaneIdentyfikacyjne": {"BrakID": 1},
                    "Rola": 10,
                },
            }
        }
    elif jst:
        root["_x_podmiot2_case"] = {
            "JST_ONLY": {
                "_x_podmiot3_jst_role": {
                    "DaneIdentyfikacyjne": {"BrakID": 1},
                    "Rola": 8,
                }
            }
        }
    elif gv:
        root["_x_podmiot2_case"] = {
            "GV_ONLY": {
                "_x_podmiot3_gv_role": {
                    "DaneIdentyfikacyjne": {"BrakID": 1},
                    "Rola": 10,
                }
            }
        }
    else:
        root["_x_podmiot2_case"] = {"NONE": {}}


def _normalize_case_payload(payload):
    merged = _deep_merge(_baseline_payload(), payload)
    _ensure_address_defaults(merged)
    _normalize_fa_structure(merged)
    _normalize_podmiot2_case(merged)
    return merged


def _merge_required(left, right):
    result = list(left or [])
    for item in right or []:
        if item not in result:
            result.append(item)
    return result


def _merge_schema_fragments(base, override):
    if not isinstance(base, dict):
        return copy.deepcopy(override)
    if not isinstance(override, dict):
        return copy.deepcopy(base)

    result = copy.deepcopy(base)
    for key, value in override.items():
        if key == "properties" and isinstance(result.get(key), dict) and isinstance(value, dict):
            merged_properties = copy.deepcopy(result[key])
            for prop_key, prop_value in value.items():
                if prop_key in merged_properties:
                    merged_properties[prop_key] = _merge_schema_fragments(
                        merged_properties[prop_key], prop_value
                    )
                else:
                    merged_properties[prop_key] = copy.deepcopy(prop_value)
            result[key] = merged_properties
        elif key == "required":
            result[key] = _merge_required(result.get(key, []), value)
        elif key in {"allOf", "anyOf", "oneOf"} and isinstance(result.get(key), list) and isinstance(value, list):
            result[key] = copy.deepcopy(result[key]) + copy.deepcopy(value)
        elif key == "items" and isinstance(result.get(key), dict) and isinstance(value, dict):
            result[key] = _merge_schema_fragments(result[key], value)
        else:
            result[key] = copy.deepcopy(value)
    return result


def _resolve_schema_ref(schema_root, ref):
    if not ref.startswith("#/"):
        raise ValueError(f"Unsupported schema ref: {ref}")

    current = schema_root
    for segment in ref[2:].split("/"):
        segment = segment.replace("~1", "/").replace("~0", "~")
        current = current[segment]
    return current


def _resolve_schema_node(schema_root, schema):
    if not isinstance(schema, dict):
        return schema

    result = copy.deepcopy(schema)
    while "$ref" in result:
        target = _resolve_schema_ref(schema_root, result.pop("$ref"))
        result = _merge_schema_fragments(target, result)
    return result


def _schema_const_mismatches(schema_root, schema, value):
    if not isinstance(value, dict) or not isinstance(schema, dict):
        return 0

    mismatches = 0
    for key, prop_schema in schema.get("properties", {}).items():
        if key not in value:
            continue
        resolved_prop_schema = _resolve_schema_node(schema_root, prop_schema)
        if isinstance(resolved_prop_schema, dict) and "const" in resolved_prop_schema:
            if value[key] != resolved_prop_schema["const"]:
                mismatches += 1
    return mismatches


def _effective_schema(schema_root, schema, value):
    resolved = _resolve_schema_node(schema_root, schema)
    if not isinstance(resolved, dict):
        return resolved

    result = copy.deepcopy(resolved)

    if "allOf" in result:
        for branch in result.pop("allOf"):
            result = _merge_schema_fragments(result, _effective_schema(schema_root, branch, value))

    for keyword in ("oneOf", "anyOf"):
        if keyword not in result:
            continue

        branches = result.pop(keyword)
        if isinstance(value, dict):
            branch_scores = []
            for index, branch in enumerate(branches):
                effective_branch = _effective_schema(schema_root, branch, value)
                branch_properties = (
                    effective_branch.get("properties", {})
                    if isinstance(effective_branch, dict)
                    else {}
                )
                branch_required = (
                    effective_branch.get("required", [])
                    if isinstance(effective_branch, dict)
                    else []
                )
                unknown_keys = 0
                if (
                    isinstance(effective_branch, dict)
                    and effective_branch.get("additionalProperties") is False
                ):
                    unknown_keys = sum(1 for key in value if key not in branch_properties)

                score = (
                    _schema_const_mismatches(schema_root, effective_branch, value),
                    sum(1 for key in branch_required if key not in value),
                    unknown_keys,
                    -sum(1 for key in value if key in branch_properties),
                    index,
                )
                branch_scores.append((score, branch))
            _, best_branch = min(branch_scores, key=lambda item: item[0])
        else:
            best_branch = branches[0]

        result = _merge_schema_fragments(
            result, _effective_schema(schema_root, best_branch, value)
        )
        break

    return result


def _canonicalize_payload_against_schema(value, schema, schema_root):
    effective_schema = _effective_schema(schema_root, schema, value)

    if isinstance(value, dict):
        properties = (
            effective_schema.get("properties", {})
            if isinstance(effective_schema, dict)
            else {}
        )
        ordered = {}
        for key in properties:
            if key in value:
                ordered[key] = _canonicalize_payload_against_schema(
                    value[key], properties[key], schema_root
                )
        for key, item in value.items():
            if key not in ordered:
                ordered[key] = copy.deepcopy(item)
        return ordered

    if isinstance(value, list):
        if isinstance(effective_schema, dict) and "items" in effective_schema:
            return [
                _canonicalize_payload_against_schema(item, effective_schema["items"], schema_root)
                for item in value
            ]
        return copy.deepcopy(value)

    return copy.deepcopy(value)


@lru_cache(maxsize=1)
def _load_schema_and_grammar():
    schema_path = Path(__file__).resolve().parents[2] / "tmp" / "ksef_schema_business_source.json"
    schema = json.loads(schema_path.read_text())
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=True)
    return schema, grammar


@pytest.mark.parametrize("case_name", list(CASES.keys()), ids=list(CASES.keys()))
def test_ksef_business_cases(case_name):
    schema, grammar = _load_schema_and_grammar()
    case = CASES[case_name]
    payload = _normalize_case_payload(case["payload"])
    payload = _canonicalize_payload_against_schema(payload, schema, schema)
    accepted = _is_grammar_accept_string(
        grammar, json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    )
    assert accepted == (case["expected"] == "VALID"), case_name
