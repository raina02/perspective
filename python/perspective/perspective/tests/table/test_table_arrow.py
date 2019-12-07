# *****************************************************************************
#
# Copyright (c) 2019, the Perspective Authors.
#
# This file is part of the Perspective library, distributed under the terms of
# the Apache License 2.0.  The full license can be found in the LICENSE file.
#

import os.path
import pyarrow as pa
from datetime import date, datetime
from pytest import mark
from perspective.table import Table

SUPERSTORE_ARROW = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "..", "examples", "simple", "superstore.arrow")

names = ["a", "b", "c", "d"]


class TestTableArrow(object):

    def test_table_arrow_loads(self):
        with open(SUPERSTORE_ARROW, mode='rb') as file:  # b is important -> binary
            tbl = Table(file.read())
            assert tbl.size() == 9994
            v = tbl.view()
            df = v.to_df()
            assert df.shape == (9994, 21)

    # streams

    def test_table_arrow_loads_int_stream(self, util):
        data = [list(range(10)) for i in range(4)]
        arrow_data = util.make_arrow(names, data)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": int,
            "b": int,
            "c": int,
            "d": int
        }
        assert tbl.view().to_dict() == {
            "a": data[0],
            "b": data[1],
            "c": data[2],
            "d": data[3]
        }

    def test_table_arrow_loads_float_stream(self, util):
        data = [
            [i for i in range(10)],
            [i * 1.5 for i in range(10)]
        ]
        arrow_data = util.make_arrow(["a", "b"], data)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": int,
            "b": float,
        }
        assert tbl.view().to_dict() == {
            "a": data[0],
            "b": data[1]
        }

    def test_table_arrow_loads_decimal_stream(self, util):
        data = [
            [i * 1000 for i in range(10)]
        ]
        arrow_data = util.make_arrow(["a"], data, types=[pa.decimal128(4)])
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": int,
        }
        assert tbl.view().to_dict() == {
            "a": data[0]
        }

    def test_table_arrow_loads_bool_stream(self, util):
        data = [
            [True if i % 2 == 0 else False for i in range(10)]
        ]
        arrow_data = util.make_arrow(["a"], data)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": bool
        }
        assert tbl.view().to_dict() == {
            "a": data[0]
        }

    def test_table_arrow_loads_date32_stream(self, util):
        data = [
            [date(2019, 2, i) for i in range(1, 11)]
        ]
        arrow_data = util.make_arrow(["a"], data, types=[pa.date32()])
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": date
        }
        assert tbl.view().to_dict() == {
            "a": [datetime(2019, 2, i) for i in range(1, 11)]
        }

    def test_table_arrow_loads_date64_stream(self, util):
        data = [
            [date(2019, 2, i) for i in range(1, 11)]
        ]
        arrow_data = util.make_arrow(["a"], data, types=[pa.date64()])
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": date
        }
        assert tbl.view().to_dict() == {
            "a": [datetime(2019, 2, i) for i in range(1, 11)]
        }

    def test_table_arrow_loads_timestamp_all_formats_stream(self, util):
        data = [
            [datetime(2019, 2, i, 9) for i in range(1, 11)],
            [datetime(2019, 2, i, 10) for i in range(1, 11)],
            [datetime(2019, 2, i, 11) for i in range(1, 11)],
            [datetime(2019, 2, i, 12) for i in range(1, 11)]
        ]
        arrow_data = util.make_arrow(
            names, data, types=[
                pa.timestamp("s"),
                pa.timestamp("ms"),
                pa.timestamp("us"),
                pa.timestamp("ns"),
            ]
        )
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": datetime,
            "b": datetime,
            "c": datetime,
            "d": datetime
        }
        assert tbl.view().to_dict() == {
            "a": data[0],
            "b": data[1],
            "c": data[2],
            "d": data[3]
        }

    def test_table_arrow_loads_string_stream(self, util):
        data = [
            [str(i) for i in range(10)]
        ]
        arrow_data = util.make_arrow(["a"], data, types=[pa.string()])
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": str
        }
        assert tbl.view().to_dict() == {
            "a": data[0]
        }

    @mark.skip
    def test_table_arrow_loads_dictionary_stream(self, util):
        data = [
            ([0, 1, 1, None], ["a", "b"]),
            ([0, 1, None, 2], ["x", "y", "z"])
        ]
        arrow_data = util.make_dictionary_arrow(["a", "b"], data)
        tbl = Table(arrow_data)

        assert tbl.size() == 4
        assert tbl.schema() == {
            "a": str,
            "b": str
        }
        assert tbl.view().to_dict() == {
            "a": ["a", "b", "b", None],
            "b": ["x", "y", None, "z"]
        }

    # legacy

    def test_table_arrow_loads_int_legacy(self, util):
        data = [list(range(10)) for i in range(4)]
        arrow_data = util.make_arrow(names, data, legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": int,
            "b": int,
            "c": int,
            "d": int
        }

    def test_table_arrow_loads_float_legacy(self, util):
        data = [
            [i for i in range(10)],
            [i * 1.5 for i in range(10)]
        ]
        arrow_data = util.make_arrow(["a", "b"], data, legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": int,
            "b": float,
        }
        assert tbl.view().to_dict() == {
            "a": data[0],
            "b": data[1]
        }

    def test_table_arrow_loads_decimal_legacy(self, util):
        data = [
            [i * 1000 for i in range(10)]
        ]
        arrow_data = util.make_arrow(
            ["a"], data, types=[pa.decimal128(4)], legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": int,
        }
        assert tbl.view().to_dict() == {
            "a": data[0]
        }

    def test_table_arrow_loads_bool_legacy(self, util):
        data = [
            [True if i % 2 == 0 else False for i in range(10)]
        ]
        arrow_data = util.make_arrow(["a"], data, legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": bool
        }
        assert tbl.view().to_dict() == {
            "a": data[0]
        }

    def test_table_arrow_loads_date32_legacy(self, util):
        data = [
            [date(2019, 2, i) for i in range(1, 11)]
        ]
        arrow_data = util.make_arrow(
            ["a"], data, types=[pa.date32()], legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": date
        }
        assert tbl.view().to_dict() == {
            "a": [datetime(2019, 2, i) for i in range(1, 11)]
        }

    def test_table_arrow_loads_date64_legacy(self, util):
        data = [
            [date(2019, 2, i) for i in range(1, 11)]
        ]
        arrow_data = util.make_arrow(
            ["a"], data, types=[pa.date64()], legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": date
        }
        assert tbl.view().to_dict() == {
            "a": [datetime(2019, 2, i) for i in range(1, 11)]
        }

    def test_table_arrow_loads_timestamp_all_formats_legacy(self, util):
        data = [
            [datetime(2019, 2, i, 9) for i in range(1, 11)],
            [datetime(2019, 2, i, 10) for i in range(1, 11)],
            [datetime(2019, 2, i, 11) for i in range(1, 11)],
            [datetime(2019, 2, i, 12) for i in range(1, 11)]
        ]
        arrow_data = util.make_arrow(
            names, data, types=[
                pa.timestamp("s"),
                pa.timestamp("ms"),
                pa.timestamp("us"),
                pa.timestamp("ns"),
            ], legacy=True
        )
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": datetime,
            "b": datetime,
            "c": datetime,
            "d": datetime
        }
        assert tbl.view().to_dict() == {
            "a": data[0],
            "b": data[1],
            "c": data[2],
            "d": data[3]
        }

    def test_table_arrow_loads_string_legacy(self, util):
        data = [
            [str(i) for i in range(10)]
        ]
        arrow_data = util.make_arrow(
            ["a"], data, types=[pa.string()], legacy=True)
        tbl = Table(arrow_data)
        assert tbl.size() == 10
        assert tbl.schema() == {
            "a": str
        }
        assert tbl.view().to_dict() == {
            "a": data[0]
        }

    @mark.skip
    def test_table_arrow_loads_dictionary_legacy(self, util):
        data = [
            ([0, 1, 1, None], ["a", "b"]),
            ([0, 1, None, 2], ["x", "y", "z"])
        ]
        arrow_data = util.make_dictionary_arrow(
            ["a", "b"], data, legacy=True)
        tbl = Table(arrow_data)

        assert tbl.size() == 4
        assert tbl.schema() == {
            "a": str,
            "b": str
        }
        assert tbl.view().to_dict() == {
            "a": ["a", "b", "b", None],
            "b": ["x", "y", None, "z"]
        }