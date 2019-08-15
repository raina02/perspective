/******************************************************************************
 *
 * Copyright (c) 2019, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */

#include <perspective/emscripten.h>

using namespace emscripten;
using namespace perspective;

namespace perspective {
namespace binding {
    /******************************************************************************
     *
     * Utility
     */

    template <typename T>
    std::vector<T>
    make_vector() {
        return std::vector<T>{};
    }

    template <>
    bool
    has_value(t_val item) {
        return (!item.isUndefined() && !item.isNull());
    }

    /******************************************************************************
     *
     * Date Parsing
     */

    t_date
    jsdate_to_t_date(t_val date) {
        return t_date(date.call<t_val>("getFullYear").as<std::int32_t>(),
            date.call<t_val>("getMonth").as<std::int32_t>(),
            date.call<t_val>("getDate").as<std::int32_t>());
    }

    t_val
    t_date_to_jsdate(t_date date) {
        t_val jsdate = t_val::global("Date").new_();
        jsdate.call<t_val>("setYear", date.year());
        jsdate.call<t_val>("setMonth", date.month());
        jsdate.call<t_val>("setDate", date.day());
        jsdate.call<t_val>("setHours", 0);
        jsdate.call<t_val>("setMinutes", 0);
        jsdate.call<t_val>("setSeconds", 0);
        jsdate.call<t_val>("setMilliseconds", 0);
        return jsdate;
    }

    /******************************************************************************
     *
     * Manipulate scalar values
     */
    t_val
    scalar_to_val(const t_tscalar& scalar, bool cast_double, bool cast_string) {
        if (!scalar.is_valid()) {
            return t_val::null();
        }
        switch (scalar.get_dtype()) {
            case DTYPE_BOOL: {
                if (scalar) {
                    return t_val(true);
                } else {
                    return t_val(false);
                }
            }
            case DTYPE_TIME: {
                if (cast_double) {
                    auto x = scalar.to_uint64();
                    double y = *reinterpret_cast<double*>(&x);
                    return t_val(y);
                } else if (cast_string) {
                    double ms = scalar.to_double();
                    t_val date = t_val::global("Date").new_(ms);
                    return date.call<t_val>("toLocaleString");
                } else {
                    return t_val(scalar.to_double());
                }
            }
            case DTYPE_FLOAT64:
            case DTYPE_FLOAT32: {
                if (cast_double) {
                    auto x = scalar.to_uint64();
                    double y = *reinterpret_cast<double*>(&x);
                    return t_val(y);
                } else {
                    return t_val(scalar.to_double());
                }
            }
            case DTYPE_DATE: {
                return t_date_to_jsdate(scalar.get<t_date>()).call<t_val>("getTime");
            }
            case DTYPE_UINT8:
            case DTYPE_UINT16:
            case DTYPE_UINT32:
            case DTYPE_INT8:
            case DTYPE_INT16:
            case DTYPE_INT32: {
                return t_val(static_cast<std::int32_t>(scalar.to_int64()));
            }
            case DTYPE_UINT64:
            case DTYPE_INT64: {
                // This could potentially lose precision
                return t_val(static_cast<std::int32_t>(scalar.to_int64()));
            }
            case DTYPE_NONE: {
                return t_val::null();
            }
            case DTYPE_STR:
            default: {
                std::wstring_convert<utf8convert_type, wchar_t> converter("", L"<Invalid>");
                return t_val(converter.from_bytes(scalar.to_string()));
            }
        }
    }

    t_val
    scalar_vec_to_val(const std::vector<t_tscalar>& scalars, std::uint32_t idx) {
        return scalar_to_val(scalars[idx]);
    }

    t_val
    scalar_vec_to_string(const std::vector<t_tscalar>& scalars, std::uint32_t idx) {
        return scalar_to_val(scalars[idx], false, true);
    }

    template <typename T, typename U>
    std::vector<U>
    vecFromArray(T& arr) {
        return vecFromJSArray<U>(arr);
    }

    template <>
    t_val
    scalar_to(const t_tscalar& scalar) {
        return scalar_to_val(scalar);
    }

    template <>
    t_val
    scalar_vec_to(const std::vector<t_tscalar>& scalars, std::uint32_t idx) {
        return scalar_vec_to_val(scalars, idx);
    }

    /**
     * Converts a std::vector<T> to a Typed Array, slicing directly from the
     * WebAssembly heap.
     */
    template <typename T>
    t_val
    vector_to_typed_array(std::vector<T>& xs) {
        T* st = &xs[0];
        uintptr_t offset = reinterpret_cast<uintptr_t>(st);
        return t_val::module_property("HEAPU8").call<t_val>(
            "slice", offset, offset + (sizeof(T) * xs.size()));
    }

    /******************************************************************************
     *
     * Write data in the Apache Arrow format
     */
    namespace arrow {

        template <>
        void
        vecFromTypedArray(
            const t_val& typedArray, void* data, std::int32_t length, const char* destType) {
            t_val memory = t_val::module_property("buffer");
            if (destType == nullptr) {
                t_val memoryView = typedArray["constructor"].new_(
                    memory, reinterpret_cast<std::uintptr_t>(data), length);
                memoryView.call<void>("set", typedArray.call<t_val>("slice", 0, length));
            } else {
                t_val memoryView = t_val::global(destType).new_(
                    memory, reinterpret_cast<std::uintptr_t>(data), length);
                memoryView.call<void>("set", typedArray.call<t_val>("slice", 0, length));
            }
        }

        template <>
        void
        fill_col_valid(t_val dcol, std::shared_ptr<t_column> col) {
            // dcol should be the Uint8Array containing the null bitmap
            t_uindex nrows = col->size();

            // arrow packs bools into a bitmap
            for (auto i = 0; i < nrows; ++i) {
                std::uint8_t elem = dcol[i / 8].as<std::uint8_t>();
                bool v = elem & (1 << (i % 8));
                col->set_valid(i, v);
            }
        }

        template <>
        void
        fill_col_dict(t_val dictvec, std::shared_ptr<t_column> col) {
            // ptaylor: This assumes the dictionary is either a Binary or Utf8 Vector. Should it
            // support other Vector types?
            t_val vdata = dictvec["values"];
            std::int32_t vsize = vdata["length"].as<std::int32_t>();
            std::vector<unsigned char> data;
            data.reserve(vsize);
            data.resize(vsize);
            vecFromTypedArray(vdata, data.data(), vsize);

            t_val voffsets = dictvec["valueOffsets"];
            std::int32_t osize = voffsets["length"].as<std::int32_t>();
            std::vector<std::int32_t> offsets;
            offsets.reserve(osize);
            offsets.resize(osize);
            vecFromTypedArray(voffsets, offsets.data(), osize);

            // Get number of dictionary entries
            std::uint32_t dsize = dictvec["length"].as<std::uint32_t>();

            t_vocab* vocab = col->_get_vocab();
            std::string elem;

            for (std::uint32_t i = 0; i < dsize; ++i) {
                std::int32_t bidx = offsets[i];
                std::size_t es = offsets[i + 1] - bidx;
                elem.assign(reinterpret_cast<char*>(data.data()) + bidx, es);
                t_uindex idx = vocab->get_interned(elem);
                // Make sure there are no duplicates in the arrow dictionary
                assert(idx == i);
            }
        }
    } // namespace arrow

    namespace js_typed_array {
        t_val ArrayBuffer = t_val::global("ArrayBuffer");
        t_val Int8Array = t_val::global("Int8Array");
        t_val Int16Array = t_val::global("Int16Array");
        t_val Int32Array = t_val::global("Int32Array");
        t_val UInt8Array = t_val::global("Uint8Array");
        t_val UInt32Array = t_val::global("Uint32Array");
        t_val Float32Array = t_val::global("Float32Array");
        t_val Float64Array = t_val::global("Float64Array");
    } // namespace js_typed_array

    template <typename T>
    const t_val typed_array = t_val::null();

    template <>
    const t_val typed_array<double> = js_typed_array::Float64Array;
    template <>
    const t_val typed_array<float> = js_typed_array::Float32Array;
    template <>
    const t_val typed_array<std::int8_t> = js_typed_array::Int8Array;
    template <>
    const t_val typed_array<std::int16_t> = js_typed_array::Int16Array;
    template <>
    const t_val typed_array<std::int32_t> = js_typed_array::Int32Array;
    template <>
    const t_val typed_array<std::uint32_t> = js_typed_array::UInt32Array;

    template <>
    double
    get_scalar<double>(t_tscalar& t) {
        return t.to_double();
    }
    template <>
    float
    get_scalar<float>(t_tscalar& t) {
        return t.to_double();
    }
    template <>
    std::uint8_t
    get_scalar<std::uint8_t>(t_tscalar& t) {
        return static_cast<std::uint8_t>(t.to_int64());
    }
    template <>
    std::int8_t
    get_scalar<std::int8_t>(t_tscalar& t) {
        return static_cast<std::int8_t>(t.to_int64());
    }
    template <>
    std::int16_t
    get_scalar<std::int16_t>(t_tscalar& t) {
        return static_cast<std::int16_t>(t.to_int64());
    }
    template <>
    std::int32_t
    get_scalar<std::int32_t>(t_tscalar& t) {
        return static_cast<std::int32_t>(t.to_int64());
    }
    template <>
    std::uint32_t
    get_scalar<std::uint32_t>(t_tscalar& t) {
        return static_cast<std::uint32_t>(t.to_int64());
    }
    template <>
    double
    get_scalar<t_date, double>(t_tscalar& t) {
        auto x = t.to_uint64();
        return *reinterpret_cast<double*>(&x);
    }

    template <typename T, typename F, typename O>
    val
    col_to_typed_array(const std::vector<t_tscalar>& data) {
        int data_size = data.size();
        std::vector<T> vals;
        vals.reserve(data.size());

        // Validity map must have a length that is a multiple of 64
        int nullSize = ceil(data_size / 64.0) * 2;
        int nullCount = 0;
        std::vector<std::uint32_t> validityMap;
        validityMap.resize(nullSize);

        for (int idx = 0; idx < data_size; idx++) {
            t_tscalar scalar = data[idx];
            if (scalar.is_valid() && scalar.get_dtype() != DTYPE_NONE) {
                vals.push_back(get_scalar<F, T>(scalar));
                // Mark the slot as non-null (valid)
                validityMap[idx / 32] |= 1 << (idx % 32);
            } else {
                vals.push_back({});
                nullCount++;
            }
        }

        t_val arr = t_val::global("Array").new_();
        arr.call<void>("push", typed_array<O>.new_(vector_to_typed_array(vals)["buffer"]));
        arr.call<void>("push", nullCount);
        arr.call<void>("push", vector_to_typed_array(validityMap));
        return arr;
    }

    template <>
    val
    col_to_typed_array<bool>(const std::vector<t_tscalar>& data) {
        int data_size = data.size();

        std::vector<std::int8_t> vals;
        vals.reserve(data.size());

        // Validity map must have a length that is a multiple of 64
        int nullSize = ceil(data_size / 64.0) * 2;
        int nullCount = 0;
        std::vector<std::uint32_t> validityMap;
        validityMap.resize(nullSize);

        for (int idx = 0; idx < data_size; idx++) {
            t_tscalar scalar = data[idx];
            if (scalar.is_valid() && scalar.get_dtype() != DTYPE_NONE) {
                // get boolean and write into array
                std::int8_t t_val = get_scalar<std::int8_t>(scalar);
                vals.push_back(t_val);
                // bit mask based on value in array
                vals[idx / 8] |= t_val << (idx % 8);
                // Mark the slot as non-null (valid)
                validityMap[idx / 32] |= 1 << (idx % 32);
            } else {
                vals.push_back({});
                nullCount++;
            }
        }

        t_val arr = t_val::global("Array").new_();
        arr.call<void>(
            "push", typed_array<std::int8_t>.new_(vector_to_typed_array(vals)["buffer"]));
        arr.call<void>("push", nullCount);
        arr.call<void>("push", vector_to_typed_array(validityMap));
        return arr;
    }

    template <>
    val
    col_to_typed_array<std::string>(const std::vector<t_tscalar>& data) {
        int data_size = data.size();

        t_vocab vocab;
        vocab.init(false);

        int nullSize = ceil(data_size / 64.0) * 2;
        int nullCount = 0;
        std::vector<std::uint32_t> validityMap; // = new std::uint32_t[nullSize];
        validityMap.resize(nullSize);
        t_val indexBuffer = js_typed_array::ArrayBuffer.new_(data_size * 4);
        t_val indexArray = js_typed_array::UInt32Array.new_(indexBuffer);

        for (int idx = 0; idx < data_size; idx++) {
            t_tscalar scalar = data[idx];
            if (scalar.is_valid() && scalar.get_dtype() != DTYPE_NONE) {
                auto adx = vocab.get_interned(scalar.to_string());
                indexArray.call<void>("fill", t_val(adx), idx, idx + 1);
                validityMap[idx / 32] |= 1 << (idx % 32);
            } else {
                nullCount++;
            }
        }
        t_val dictBuffer = js_typed_array::ArrayBuffer.new_(
            vocab.get_vlendata()->size() - vocab.get_vlenidx());
        t_val dictArray = js_typed_array::UInt8Array.new_(dictBuffer);
        std::vector<std::uint32_t> offsets;
        offsets.reserve(vocab.get_vlenidx() + 1);
        std::uint32_t index = 0;
        for (auto i = 0; i < vocab.get_vlenidx(); i++) {
            const char* str = vocab.unintern_c(i);
            offsets.push_back(index);
            while (*str) {
                dictArray.call<void>("fill", t_val(*str++), index, index + 1);
                index++;
            }
        }
        offsets.push_back(index);

        t_val arr = t_val::global("Array").new_();
        arr.call<void>("push", dictArray);
        arr.call<void>(
            "push", js_typed_array::UInt32Array.new_(vector_to_typed_array(offsets)["buffer"]));
        arr.call<void>("push", indexArray);
        arr.call<void>("push", nullCount);
        arr.call<void>("push", vector_to_typed_array(validityMap));
        return arr;
    }

    t_val
    col_to_js_typed_array(const std::vector<t_tscalar>& data, t_dtype dtype, t_index idx) {
        switch (dtype) {
            case DTYPE_INT8: {
                return col_to_typed_array<std::int8_t>(data);
            } break;
            case DTYPE_INT16: {
                return col_to_typed_array<std::int16_t>(data);
            } break;
            case DTYPE_DATE:
            case DTYPE_TIME: {
                return col_to_typed_array<double, t_date, std::int32_t>(data);
            } break;
            case DTYPE_INT32:
            case DTYPE_UINT32: {
                return col_to_typed_array<std::uint32_t>(data);
            } break;
            case DTYPE_INT64: {
                return col_to_typed_array<std::int32_t>(data);
            } break;
            case DTYPE_FLOAT32: {
                return col_to_typed_array<float>(data);
            } break;
            case DTYPE_FLOAT64: {
                return col_to_typed_array<double>(data);
            } break;
            case DTYPE_BOOL: {
                return col_to_typed_array<bool>(data);
            } break;
            case DTYPE_STR: {
                return col_to_typed_array<std::string>(data);
            } break;
            default: {
                PSP_COMPLAIN_AND_ABORT("Unhandled aggregate type");
                return t_val::undefined();
            }
        }
    }

    /******************************************************************************
     *
     * Data accessor API
     */

    std::vector<std::string>
    get_column_names(t_val data, std::int32_t format) {
        std::vector<std::string> names;
        t_val Object = t_val::global("Object");

        if (format == 0) {
            std::int32_t max_check = 50;
            t_val data_names = Object.call<t_val>("keys", data[0]);
            names = vecFromArray<t_val, std::string>(data_names);
            std::int32_t check_index = std::min(max_check, data["length"].as<std::int32_t>());

            for (auto ix = 0; ix < check_index; ix++) {
                t_val next = Object.call<t_val>("keys", data[ix]);

                if (names.size() != next["length"].as<std::int32_t>()) {
                    auto old_size = names.size();
                    auto new_names = vecFromJSArray<std::string>(next);
                    if (max_check == 50) {
                        std::cout << "Data parse warning: Array data has inconsistent rows"
                                  << std::endl;
                    }

                    for (auto s = new_names.begin(); s != new_names.end(); ++s) {
                        if (std::find(names.begin(), names.end(), *s) == names.end()) {
                            names.push_back(*s);
                        }
                    }

                    std::cout << "Extended from " << old_size << "to " << names.size()
                              << std::endl;
                    max_check *= 2;
                }
            }
        } else if (format == 1 || format == 2) {
            t_val keys = Object.call<t_val>("keys", data);
            names = vecFromArray<t_val, std::string>(keys);
        }

        return names;
    }

    t_dtype
    infer_type(t_val x, t_val date_validator) {
        std::string jstype = x.typeOf().as<std::string>();
        t_dtype t = t_dtype::DTYPE_STR;

        // Unwrap numbers inside strings
        t_val x_number = t_val::global("Number").call<t_val>("call", t_val::object(), x);
        bool number_in_string = (jstype == "string") && (x["length"].as<std::int32_t>() != 0)
            && (!t_val::global("isNaN").call<bool>("call", t_val::object(), x_number));

        if (x.isNull()) {
            t = t_dtype::DTYPE_NONE;
        } else if (jstype == "number" || number_in_string) {
            if (number_in_string) {
                x = x_number;
            }
            double x_float64 = x.as<double>();
            if ((std::fmod(x_float64, 1.0) == 0.0) && (x_float64 < 10000.0)
                && (x_float64 != 0.0)) {
                t = t_dtype::DTYPE_INT32;
            } else {
                t = t_dtype::DTYPE_FLOAT64;
            }
        } else if (jstype == "boolean") {
            t = t_dtype::DTYPE_BOOL;
        } else if (x.instanceof (t_val::global("Date"))) {
            std::int32_t hours = x.call<t_val>("getHours").as<std::int32_t>();
            std::int32_t minutes = x.call<t_val>("getMinutes").as<std::int32_t>();
            std::int32_t seconds = x.call<t_val>("getSeconds").as<std::int32_t>();
            std::int32_t milliseconds = x.call<t_val>("getMilliseconds").as<std::int32_t>();

            if (hours == 0 && minutes == 0 && seconds == 0 && milliseconds == 0) {
                t = t_dtype::DTYPE_DATE;
            } else {
                t = t_dtype::DTYPE_TIME;
            }
        } else if (jstype == "string") {
            if (date_validator.call<t_val>("call", t_val::object(), x).as<bool>()) {
                t = t_dtype::DTYPE_TIME;
            } else {
                std::string lower = x.call<t_val>("toLowerCase").as<std::string>();
                if (lower == "true" || lower == "false") {
                    t = t_dtype::DTYPE_BOOL;
                } else {
                    t = t_dtype::DTYPE_STR;
                }
            }
        }

        return t;
    }

    t_dtype
    get_data_type(
        t_val data, std::int32_t format, const std::string& name, t_val date_validator) {
        std::int32_t i = 0;
        boost::optional<t_dtype> inferredType;

        if (format == 0) {
            // loop parameters differ slightly so rewrite the loop
            while (!inferredType.is_initialized() && i < 100
                && i < data["length"].as<std::int32_t>()) {
                if (data[i].call<t_val>("hasOwnProperty", name).as<bool>() == true) {
                    if (!data[i][name].isNull()) {
                        inferredType = infer_type(data[i][name], date_validator);
                    } else {
                        inferredType = t_dtype::DTYPE_STR;
                    }
                }

                i++;
            }
        } else if (format == 1) {
            while (!inferredType.is_initialized() && i < 100
                && i < data[name]["length"].as<std::int32_t>()) {
                if (!data[name][i].isNull()) {
                    inferredType = infer_type(data[name][i], date_validator);
                } else {
                    inferredType = t_dtype::DTYPE_STR;
                }

                i++;
            }
        }

        if (!inferredType.is_initialized()) {
            return t_dtype::DTYPE_STR;
        } else {
            return inferredType.get();
        }
    }

    std::vector<t_dtype>
    get_data_types(t_val data, std::int32_t format, const std::vector<std::string>& names,
        t_val date_validator) {
        if (names.size() == 0) {
            PSP_COMPLAIN_AND_ABORT("Cannot determine data types without column names!");
        }

        std::vector<t_dtype> types;

        if (format == 2) {
            t_val keys = t_val::global("Object").template call<t_val>("keys", data);
            std::vector<std::string> data_names = vecFromArray<t_val, std::string>(keys);

            for (const std::string& name : data_names) {
                std::string value = data[name].as<std::string>();
                t_dtype type;

                if (value == "integer") {
                    type = t_dtype::DTYPE_INT32;
                } else if (value == "float") {
                    type = t_dtype::DTYPE_FLOAT64;
                } else if (value == "string") {
                    type = t_dtype::DTYPE_STR;
                } else if (value == "boolean") {
                    type = t_dtype::DTYPE_BOOL;
                } else if (value == "datetime") {
                    type = t_dtype::DTYPE_TIME;
                } else if (value == "date") {
                    type = t_dtype::DTYPE_DATE;
                } else {
                    PSP_COMPLAIN_AND_ABORT(
                        "Unknown type '" + value + "' for key '" + name + "'");
                }

                types.push_back(type);
            }

            return types;
        } else {
            for (const std::string& name : names) {
                t_dtype type = get_data_type(data, format, name, date_validator);
                types.push_back(type);
            }
        }

        return types;
    }

    /******************************************************************************
     *
     * Fill columns with data
     */

    void
    _fill_col_int64(t_data_accessor accessor, std::shared_ptr<t_column> col, std::string name,
        std::int32_t cidx, t_dtype type, bool is_arrow, bool is_update) {
        t_uindex nrows = col->size();

        if (is_arrow) {
            t_val data = accessor["values"];
            // arrow packs 64 bit into two 32 bit ints
            arrow::vecFromTypedArray(data, col->get_nth<std::int64_t>(0), nrows * 2);
        } else {
            PSP_COMPLAIN_AND_ABORT(
                "Unreachable - can't have DTYPE_INT64 column from non-arrow data");
        }
    }

    void
    _fill_col_time(t_data_accessor accessor, std::shared_ptr<t_column> col, std::string name,
        std::int32_t cidx, t_dtype type, bool is_arrow, bool is_update) {
        t_uindex nrows = col->size();

        if (is_arrow) {
            t_val data = accessor["values"];
            // arrow packs 64 bit into two 32 bit ints
            arrow::vecFromTypedArray(data, col->get_nth<t_time>(0), nrows * 2);

            std::int8_t unit = accessor["type"]["unit"].as<std::int8_t>();
            if (unit != /* Arrow.enum_.TimeUnit.MILLISECOND */ 1) {
                // Slow path - need to convert each value
                std::int64_t factor = 1;
                if (unit == /* Arrow.enum_.TimeUnit.NANOSECOND */ 3) {
                    factor = 1e6;
                } else if (unit == /* Arrow.enum_.TimeUnit.MICROSECOND */ 2) {
                    factor = 1e3;
                }
                for (auto i = 0; i < nrows; ++i) {
                    col->set_nth<std::int64_t>(i, *(col->get_nth<std::int64_t>(i)) / factor);
                }
            }
        } else {
            for (auto i = 0; i < nrows; ++i) {
                t_val item = accessor.call<t_val>("marshal", cidx, i, type);

                if (item.isUndefined())
                    continue;

                if (item.isNull()) {
                    if (is_update) {
                        col->unset(i);
                    } else {
                        col->clear(i);
                    }
                    continue;
                }

                auto elem = static_cast<std::int64_t>(
                    item.call<t_val>("getTime").as<double>()); // dcol[i].as<T>();
                col->set_nth(i, elem);
            }
        }
    }

    void
    _fill_col_date(t_data_accessor accessor, std::shared_ptr<t_column> col, std::string name,
        std::int32_t cidx, t_dtype type, bool is_arrow, bool is_update) {
        t_uindex nrows = col->size();

        if (is_arrow) {
            // t_val data = dcol["values"];
            // // arrow packs 64 bit into two 32 bit ints
            // arrow::vecFromTypedArray(data, col->get_nth<t_time>(0), nrows * 2);

            // std::int8_t unit = dcol["type"]["unit"].as<std::int8_t>();
            // if (unit != /* Arrow.enum_.TimeUnit.MILLISECOND */ 1) {
            //     // Slow path - need to convert each value
            //     std::int64_t factor = 1;
            //     if (unit == /* Arrow.enum_.TimeUnit.NANOSECOND */ 3) {
            //         factor = 1e6;
            //     } else if (unit == /* Arrow.enum_.TimeUnit.MICROSECOND */ 2) {
            //         factor = 1e3;
            //     }
            //     for (auto i = 0; i < nrows; ++i) {
            //         col->set_nth<std::int32_t>(i, *(col->get_nth<std::int32_t>(i)) / factor);
            //     }
            // }
        } else {
            for (auto i = 0; i < nrows; ++i) {
                t_val item = accessor.call<t_val>("marshal", cidx, i, type);

                if (item.isUndefined())
                    continue;

                if (item.isNull()) {
                    if (is_update) {
                        col->unset(i);
                    } else {
                        col->clear(i);
                    }
                    continue;
                }

                col->set_nth(i, jsdate_to_t_date(item));
            }
        }
    }

    void
    _fill_col_bool(t_data_accessor accessor, std::shared_ptr<t_column> col, std::string name,
        std::int32_t cidx, t_dtype type, bool is_arrow, bool is_update) {
        t_uindex nrows = col->size();

        if (is_arrow) {
            // bools are stored using a bit mask
            t_val data = accessor["values"];
            for (auto i = 0; i < nrows; ++i) {
                t_val item = data[i / 8];

                if (item.isUndefined()) {
                    continue;
                }

                if (item.isNull()) {
                    if (is_update) {
                        col->unset(i);
                    } else {
                        col->clear(i);
                    }
                    continue;
                }

                std::uint8_t elem = item.as<std::uint8_t>();
                bool v = elem & (1 << (i % 8));
                col->set_nth(i, v);
            }
        } else {
            for (auto i = 0; i < nrows; ++i) {
                t_val item = accessor.call<t_val>("marshal", cidx, i, type);

                if (item.isUndefined())
                    continue;

                if (item.isNull()) {
                    if (is_update) {
                        col->unset(i);
                    } else {
                        col->clear(i);
                    }
                    continue;
                }

                auto elem = item.as<bool>();
                col->set_nth(i, elem);
            }
        }
    }

    void
    _fill_col_string(t_data_accessor accessor, std::shared_ptr<t_column> col, std::string name,
        std::int32_t cidx, t_dtype type, bool is_arrow, bool is_update) {

        t_uindex nrows = col->size();

        if (is_arrow) {
            if (accessor["constructor"]["name"].as<std::string>() == "DictionaryVector") {

                t_val dictvec = accessor["dictionary"];
                arrow::fill_col_dict(dictvec, col);

                // Now process index into dictionary

                // Perspective stores string indices in a 32bit unsigned array
                // Javascript's typed arrays handle copying from various bitwidth arrays
                // properly
                t_val vkeys = accessor["indices"]["values"];
                arrow::vecFromTypedArray(
                    vkeys, col->get_nth<t_uindex>(0), nrows, "Uint32Array");

            } else if (accessor["constructor"]["name"].as<std::string>() == "Utf8Vector"
                || accessor["constructor"]["name"].as<std::string>() == "BinaryVector") {

                t_val vdata = accessor["values"];
                std::int32_t vsize = vdata["length"].as<std::int32_t>();
                std::vector<std::uint8_t> data;
                data.reserve(vsize);
                data.resize(vsize);
                arrow::vecFromTypedArray(vdata, data.data(), vsize);

                t_val voffsets = accessor["valueOffsets"];
                std::int32_t osize = voffsets["length"].as<std::int32_t>();
                std::vector<std::int32_t> offsets;
                offsets.reserve(osize);
                offsets.resize(osize);
                arrow::vecFromTypedArray(voffsets, offsets.data(), osize);

                std::string elem;

                for (std::int32_t i = 0; i < nrows; ++i) {
                    std::int32_t bidx = offsets[i];
                    std::size_t es = offsets[i + 1] - bidx;
                    elem.assign(reinterpret_cast<char*>(data.data()) + bidx, es);
                    col->set_nth(i, elem);
                }
            }
        } else {
            for (auto i = 0; i < nrows; ++i) {
                t_val item = accessor.call<t_val>("marshal", cidx, i, type);

                if (item.isUndefined())
                    continue;

                if (item.isNull()) {
                    if (is_update) {
                        col->unset(i);
                    } else {
                        col->clear(i);
                    }
                    continue;
                }

                std::wstring welem = item.as<std::wstring>();
                std::wstring_convert<utf16convert_type, wchar_t> converter;
                std::string elem = converter.to_bytes(welem);
                col->set_nth(i, elem);
            }
        }
    }

    void
    _fill_col_numeric(t_data_accessor accessor, t_data_table& tbl,
        std::shared_ptr<t_column> col, std::string name, std::int32_t cidx, t_dtype type,
        bool is_arrow, bool is_update) {
        t_uindex nrows = col->size();

        if (is_arrow) {
            t_val data = accessor["values"];

            switch (type) {
                case DTYPE_INT8: {
                    arrow::vecFromTypedArray(data, col->get_nth<std::int8_t>(0), nrows);
                } break;
                case DTYPE_INT16: {
                    arrow::vecFromTypedArray(data, col->get_nth<std::int16_t>(0), nrows);
                } break;
                case DTYPE_INT32: {
                    arrow::vecFromTypedArray(data, col->get_nth<std::int32_t>(0), nrows);
                } break;
                case DTYPE_FLOAT32: {
                    arrow::vecFromTypedArray(data, col->get_nth<float>(0), nrows);
                } break;
                case DTYPE_FLOAT64: {
                    arrow::vecFromTypedArray(data, col->get_nth<double>(0), nrows);
                } break;
                default:
                    break;
            }
        } else {
            for (auto i = 0; i < nrows; ++i) {
                t_val item = accessor.call<t_val>("marshal", cidx, i, type);

                if (item.isUndefined())
                    continue;

                if (item.isNull()) {
                    if (is_update) {
                        col->unset(i);
                    } else {
                        col->clear(i);
                    }
                    continue;
                }

                switch (type) {
                    case DTYPE_INT8: {
                        col->set_nth(i, item.as<std::int8_t>());
                    } break;
                    case DTYPE_INT16: {
                        col->set_nth(i, item.as<std::int16_t>());
                    } break;
                    case DTYPE_INT32: {
                        // This handles cases where a long sequence of e.g. 0 precedes a clearly
                        // float value in an inferred column. Would not be needed if the type
                        // inference checked the entire column/we could reset parsing.
                        double fval = item.as<double>();
                        if (fval > 2147483647 || fval < -2147483648) {
                            std::cout << "Promoting to float" << std::endl;
                            tbl.promote_column(name, DTYPE_FLOAT64, i, true);
                            col = tbl.get_column(name);
                            type = DTYPE_FLOAT64;
                            col->set_nth(i, fval);
                        } else if (isnan(fval)) {
                            std::cout << "Promoting to string" << std::endl;
                            tbl.promote_column(name, DTYPE_STR, i, false);
                            col = tbl.get_column(name);
                            _fill_col_string(
                                accessor, col, name, cidx, DTYPE_STR, is_arrow, is_update);
                            return;
                        } else {
                            col->set_nth(i, static_cast<std::int32_t>(fval));
                        }
                    } break;
                    case DTYPE_FLOAT32: {
                        col->set_nth(i, item.as<float>());
                    } break;
                    case DTYPE_FLOAT64: {
                        col->set_nth(i, item.as<double>());
                    } break;
                    default:
                        break;
                }
            }
        }
    }

    template <>
    void
    set_column_nth(t_column* col, t_uindex idx, t_val value) {

        // Check if the value is a javascript null
        if (value.isNull()) {
            col->unset(idx);
            return;
        }

        switch (col->get_dtype()) {
            case DTYPE_BOOL: {
                col->set_nth<bool>(idx, value.as<bool>(), STATUS_VALID);
                break;
            }
            case DTYPE_FLOAT64: {
                col->set_nth<double>(idx, value.as<double>(), STATUS_VALID);
                break;
            }
            case DTYPE_FLOAT32: {
                col->set_nth<float>(idx, value.as<float>(), STATUS_VALID);
                break;
            }
            case DTYPE_UINT32: {
                col->set_nth<std::uint32_t>(idx, value.as<std::uint32_t>(), STATUS_VALID);
                break;
            }
            case DTYPE_UINT64: {
                col->set_nth<std::uint64_t>(idx, value.as<std::uint64_t>(), STATUS_VALID);
                break;
            }
            case DTYPE_INT32: {
                col->set_nth<std::int32_t>(idx, value.as<std::int32_t>(), STATUS_VALID);
                break;
            }
            case DTYPE_INT64: {
                col->set_nth<std::int64_t>(idx, value.as<std::int64_t>(), STATUS_VALID);
                break;
            }
            case DTYPE_STR: {
                std::wstring welem = value.as<std::wstring>();

                std::wstring_convert<utf16convert_type, wchar_t> converter;
                std::string elem = converter.to_bytes(welem);
                col->set_nth(idx, elem, STATUS_VALID);
                break;
            }
            case DTYPE_DATE: {
                col->set_nth<t_date>(idx, jsdate_to_t_date(value), STATUS_VALID);
                break;
            }
            case DTYPE_TIME: {
                col->set_nth<std::int64_t>(
                    idx, static_cast<std::int64_t>(value.as<double>()), STATUS_VALID);
                break;
            }
            case DTYPE_UINT8:
            case DTYPE_UINT16:
            case DTYPE_INT8:
            case DTYPE_INT16:
            default: {
                // Other types not implemented
            }
        }
    }

    template <>
    void
    table_add_computed_column(t_data_table& table, t_val computed_defs) {
        auto vcomputed_defs = vecFromArray<t_val, t_val>(computed_defs);
        for (auto i = 0; i < vcomputed_defs.size(); ++i) {
            t_val coldef = vcomputed_defs[i];
            std::string name = coldef["column"].as<std::string>();
            t_val inputs = coldef["inputs"];
            t_val func = coldef["func"];
            t_val type = coldef["type"];

            std::string stype;

            if (type.isUndefined()) {
                stype = "string";
            } else {
                stype = type.as<std::string>();
            }

            t_dtype dtype;
            if (stype == "integer") {
                dtype = DTYPE_INT32;
            } else if (stype == "float") {
                dtype = DTYPE_FLOAT64;
            } else if (stype == "boolean") {
                dtype = DTYPE_BOOL;
            } else if (stype == "date") {
                dtype = DTYPE_DATE;
            } else if (stype == "datetime") {
                dtype = DTYPE_TIME;
            } else {
                dtype = DTYPE_STR;
            }

            // Get list of input column names
            auto icol_names = vecFromArray<t_val, std::string>(inputs);

            // Get t_column* for all input columns
            std::vector<const t_column*> icols;
            for (const auto& cc : icol_names) {
                icols.push_back(table._get_column(cc));
            }

            int arity = icols.size();

            // Add new column
            t_column* out = table.add_column(name, dtype, true);

            t_val i1 = t_val::undefined(), i2 = t_val::undefined(), i3 = t_val::undefined(),
                  i4 = t_val::undefined();

            t_uindex size = table.size();
            for (t_uindex ridx = 0; ridx < size; ++ridx) {
                t_val value = t_val::undefined();

                switch (arity) {
                    case 0: {
                        value = func();
                        break;
                    }
                    case 1: {
                        i1 = scalar_to_val(icols[0]->get_scalar(ridx));
                        if (!i1.isNull()) {
                            value = func(i1);
                        }
                        break;
                    }
                    case 2: {
                        i1 = scalar_to_val(icols[0]->get_scalar(ridx));
                        i2 = scalar_to_val(icols[1]->get_scalar(ridx));
                        if (!i1.isNull() && !i2.isNull()) {
                            value = func(i1, i2);
                        }
                        break;
                    }
                    case 3: {
                        i1 = scalar_to_val(icols[0]->get_scalar(ridx));
                        i2 = scalar_to_val(icols[1]->get_scalar(ridx));
                        i3 = scalar_to_val(icols[2]->get_scalar(ridx));
                        if (!i1.isNull() && !i2.isNull() && !i3.isNull()) {
                            value = func(i1, i2, i3);
                        }
                        break;
                    }
                    case 4: {
                        i1 = scalar_to_val(icols[0]->get_scalar(ridx));
                        i2 = scalar_to_val(icols[1]->get_scalar(ridx));
                        i3 = scalar_to_val(icols[2]->get_scalar(ridx));
                        i4 = scalar_to_val(icols[3]->get_scalar(ridx));
                        if (!i1.isNull() && !i2.isNull() && !i3.isNull() && !i4.isNull()) {
                            value = func(i1, i2, i3, i4);
                        }
                        break;
                    }
                    default: {
                        // Don't handle other arity values
                        break;
                    }
                }

                if (!value.isUndefined()) {
                    set_column_nth(out, ridx, value);
                }
            }
        }
    }

    /******************************************************************************
     *
     * Fill tables with data
     */

    void
    _fill_data(t_data_table& tbl, t_data_accessor accessor, std::vector<std::string> col_names,
        std::vector<t_dtype> data_types, std::uint32_t offset, bool is_arrow, bool is_update) {

        for (auto cidx = 0; cidx < col_names.size(); ++cidx) {
            auto name = col_names[cidx];
            auto col = tbl.get_column(name);
            auto col_type = data_types[cidx];

            t_val dcol = t_val::undefined();

            if (is_arrow) {
                dcol = accessor["cdata"][cidx];
            } else {
                dcol = accessor;
            }

            switch (col_type) {
                case DTYPE_INT64: {
                    _fill_col_int64(dcol, col, name, cidx, col_type, is_arrow, is_update);
                } break;
                case DTYPE_BOOL: {
                    _fill_col_bool(dcol, col, name, cidx, col_type, is_arrow, is_update);
                } break;
                case DTYPE_DATE: {
                    _fill_col_date(dcol, col, name, cidx, col_type, is_arrow, is_update);
                } break;
                case DTYPE_TIME: {
                    _fill_col_time(dcol, col, name, cidx, col_type, is_arrow, is_update);
                } break;
                case DTYPE_STR: {
                    _fill_col_string(dcol, col, name, cidx, col_type, is_arrow, is_update);
                } break;
                case DTYPE_NONE: {
                    break;
                }
                default:
                    _fill_col_numeric(
                        dcol, tbl, col, name, cidx, col_type, is_arrow, is_update);
            }

            if (is_arrow) {
                // Fill validity bitmap
                std::uint32_t null_count = dcol["nullCount"].as<std::uint32_t>();

                if (null_count == 0) {
                    col->valid_raw_fill();
                } else {
                    t_val validity = dcol["nullBitmap"];
                    arrow::fill_col_valid(validity, col);
                }
            }
        }
    }

    /******************************************************************************
     *
     * Table API
     */

    template <>
    std::shared_ptr<Table>
    make_table(t_val table, t_data_accessor accessor, t_val computed, std::uint32_t offset,
        std::uint32_t limit, std::string index, t_op op, bool is_arrow) {
        bool is_update = op == OP_UPDATE;
        bool is_delete = op == OP_DELETE;
        std::vector<std::string> column_names;
        std::vector<t_dtype> data_types;

        // Determine metadata
        if (is_arrow || (is_update || is_delete)) {
            t_val names = accessor["names"];
            t_val types = accessor["types"];
            column_names = vecFromArray<t_val, std::string>(names);
            data_types = vecFromArray<t_val, t_dtype>(types);
        } else {
            // Infer names and types
            t_val data = accessor["data"];
            std::int32_t format = accessor["format"].as<std::int32_t>();
            column_names = get_column_names(data, format);
            data_types = get_data_types(data, format, column_names, accessor["date_validator"]);
        }

        // Check if index is valid after getting column names
        bool valid_index
            = std::find(column_names.begin(), column_names.end(), index) != column_names.end();
        if (index != "" && !valid_index) {
            PSP_COMPLAIN_AND_ABORT("Specified index '" + index + "' does not exist in data.")
        }

        bool table_initialized = has_value(table);
        std::shared_ptr<Table> tbl;

        if (table_initialized) {
            tbl = table.as<std::shared_ptr<Table>>();
            auto current_gnode = tbl->get_gnode();
            tbl->update(column_names, data_types, offset, limit, index, op, is_arrow);

            // use gnode metadata to help decide if we need to update
            is_update = (is_update || current_gnode->mapping_size() > 0);

            // if performing an arrow schema update, promote columns
            auto current_data_table = current_gnode->get_table();

            if (is_arrow && is_update && current_data_table->size() == 0) {
                auto current_schema = current_data_table->get_schema();
                for (auto idx = 0; idx < current_schema.m_types.size(); ++idx) {
                    if (data_types[idx] == DTYPE_INT64) {
                        std::cout << "Promoting int64 `" << column_names[idx] << "`"
                                  << std::endl;
                        current_gnode->promote_column(column_names[idx], DTYPE_INT64);
                    }
                }
            }
        } else {
            std::shared_ptr<t_pool> pool = std::make_shared<t_pool>();
            tbl = std::make_shared<Table>(
                pool, column_names, data_types, offset, limit, index, op, is_arrow);
        }

        std::uint32_t row_count = accessor["row_count"].as<std::int32_t>();
        t_data_table data_table(t_schema(column_names, data_types));
        data_table.init();
        data_table.extend(row_count);

        _fill_data(data_table, accessor, column_names, data_types, offset, is_arrow, is_update);

        if (!computed.isUndefined()) {
            table_add_computed_column(data_table, computed);
        }

        tbl->init(data_table);
        return tbl;
    }

    template <>
    std::shared_ptr<Table>
    make_computed_table(std::shared_ptr<Table> table, t_val computed) {
        auto gnode = table->get_gnode();

        t_data_table* data_table = gnode->_get_pkeyed_table();
        table_add_computed_column(*data_table, computed);
        table->replace_data_table(data_table);

        return table;
    }

    /******************************************************************************
     *
     * View API
     */

    template <>
    bool
    is_valid_filter(t_dtype type, t_val date_parser, t_val filter_term, t_val filter_operand) {
        std::string comp_str = filter_operand.as<std::string>();
        t_filter_op comp = str_to_filter_op(comp_str);

        if (comp == t_filter_op::FILTER_OP_IS_NULL
            || comp == t_filter_op::FILTER_OP_IS_NOT_NULL) {
            return true;
        } else if (type == DTYPE_DATE || type == DTYPE_TIME) {
            t_val parsed_date = date_parser.call<t_val>("parse", filter_term);
            return has_value(parsed_date);
        } else {
            return has_value(filter_term);
        }
    };

    template <>
    std::tuple<std::string, std::string, std::vector<t_tscalar>>
    make_filter_term(t_dtype type, t_val date_parser, std::vector<t_val> filter) {
        std::string col = filter[0].as<std::string>();
        std::string comp_str = filter[1].as<std::string>();
        t_filter_op comp = str_to_filter_op(comp_str);
        std::vector<t_tscalar> terms;

        switch (comp) {
            case FILTER_OP_NOT_IN:
            case FILTER_OP_IN: {
                std::vector<std::string> filter_terms
                    = vecFromArray<t_val, std::string>(filter[2]);
                for (auto term : filter_terms) {
                    terms.push_back(mktscalar(get_interned_cstr(term.c_str())));
                }
            } break;
            case FILTER_OP_IS_NULL:
            case FILTER_OP_IS_NOT_NULL: {
                terms.push_back(mktscalar(0));
            } break;
            default: {
                switch (type) {
                    case DTYPE_INT32: {
                        terms.push_back(mktscalar(filter[2].as<std::int32_t>()));
                    } break;
                    case DTYPE_INT64:
                    case DTYPE_FLOAT64: {
                        terms.push_back(mktscalar(filter[2].as<double>()));
                    } break;
                    case DTYPE_BOOL: {
                        terms.push_back(mktscalar(filter[2].as<bool>()));
                    } break;
                    case DTYPE_DATE: {
                        t_val parsed_date = date_parser.call<t_val>("parse", filter[2]);
                        terms.push_back(mktscalar(jsdate_to_t_date(parsed_date)));
                    } break;
                    case DTYPE_TIME: {
                        t_val parsed_date = date_parser.call<t_val>("parse", filter[2]);
                        terms.push_back(mktscalar(t_time(static_cast<std::int64_t>(
                            parsed_date.call<t_val>("getTime").as<double>()))));
                    } break;
                    default: {
                        terms.push_back(
                            mktscalar(get_interned_cstr(filter[2].as<std::string>().c_str())));
                    }
                }
            }
        }

        return std::make_tuple(col, comp_str, terms);
    }

    template <>
    t_view_config
    make_view_config(const t_schema& schema, t_val date_parser, t_val config) {
        // extract vectors from JS, where they were created
        auto row_pivots = config.call<std::vector<std::string>>("get_row_pivots");
        auto column_pivots = config.call<std::vector<std::string>>("get_column_pivots");
        auto columns = config.call<std::vector<std::string>>("get_columns");
        auto sort = config.call<std::vector<std::vector<std::string>>>("get_sort");
        auto filter_op = config["filter_op"].as<std::string>();

        // aggregates require manual parsing - std::maps read from JS are empty
        t_val j_aggregate_keys
            = t_val::global("Object").call<t_val>("keys", config["aggregates"]);
        auto aggregate_names = vecFromArray<t_val, std::string>(j_aggregate_keys);

        tsl::ordered_map<std::string, std::string> aggregates;
        for (const auto& name : aggregate_names) {
            aggregates[name] = config["aggregates"][name].as<std::string>();
        };

        bool column_only = false;

        // make sure that primary keys are created for column-only views
        if (row_pivots.size() == 0 && column_pivots.size() > 0) {
            row_pivots.push_back("psp_okey");
            column_only = true;
        }

        // construct filters with filter terms, and fill the vector of tuples
        auto js_filter = config.call<std::vector<std::vector<t_val>>>("get_filter");
        std::vector<std::tuple<std::string, std::string, std::vector<t_tscalar>>> filter;

        for (auto f : js_filter) {
            t_dtype type = schema.get_dtype(f.at(0).as<std::string>());

            // validate the filter before it goes into the core engine
            t_val filter_term = t_val::null();
            if (f.size() > 2) {
                filter_term = f.at(2);
            }
            if (is_valid_filter(type, date_parser, filter_term, f.at(1))) {
                filter.push_back(make_filter_term(type, date_parser, f));
            }
        }

        // create the `t_view_config`
        t_view_config view_config(row_pivots, column_pivots, aggregates, columns, filter, sort,
            filter_op, column_only);

        // transform primitive values into abstractions that the engine can use
        view_config.init(schema);

        // set pivot depths if provided
        if (has_value(config["row_pivot_depth"])) {
            view_config.set_row_pivot_depth(config["row_pivot_depth"].as<std::int32_t>());
        }

        if (has_value(config["column_pivot_depth"])) {
            view_config.set_column_pivot_depth(config["column_pivot_depth"].as<std::int32_t>());
        }

        return view_config;
    }

    template <typename CTX_T>
    std::shared_ptr<View<CTX_T>>
    make_view(std::shared_ptr<Table> table, std::string name, std::string separator,
        t_val view_config, t_val date_parser) {
        auto schema = table->get_schema();
        t_view_config config = make_view_config<t_val>(schema, date_parser, view_config);

        auto ctx = make_context<CTX_T>(table, schema, config, name);

        auto view_ptr = std::make_shared<View<CTX_T>>(table, ctx, name, separator, config);

        return view_ptr;
    }

    /******************************************************************************
     *
     * Context API
     */

    template <>
    std::shared_ptr<t_ctx0>
    make_context(std::shared_ptr<Table> table, const t_schema& schema,
        const t_view_config& view_config, std::string name) {
        auto columns = view_config.get_columns();
        auto filter_op = view_config.get_filter_op();
        auto fterm = view_config.get_fterm();
        auto sortspec = view_config.get_sortspec();

        auto cfg = t_config(columns, filter_op, fterm);
        auto ctx0 = std::make_shared<t_ctx0>(schema, cfg);
        ctx0->init();
        ctx0->sort_by(sortspec);

        auto pool = table->get_pool();
        auto gnode = table->get_gnode();
        pool->register_context(gnode->get_id(), name, ZERO_SIDED_CONTEXT,
            reinterpret_cast<std::uintptr_t>(ctx0.get()));

        return ctx0;
    }

    template <>
    std::shared_ptr<t_ctx1>
    make_context(std::shared_ptr<Table> table, const t_schema& schema,
        const t_view_config& view_config, std::string name) {
        auto row_pivots = view_config.get_row_pivots();
        auto aggspecs = view_config.get_aggspecs();
        auto filter_op = view_config.get_filter_op();
        auto fterm = view_config.get_fterm();
        auto sortspec = view_config.get_sortspec();
        auto row_pivot_depth = view_config.get_row_pivot_depth();

        auto cfg = t_config(row_pivots, aggspecs, filter_op, fterm);
        auto ctx1 = std::make_shared<t_ctx1>(schema, cfg);

        ctx1->init();
        ctx1->sort_by(sortspec);

        auto pool = table->get_pool();
        auto gnode = table->get_gnode();
        pool->register_context(gnode->get_id(), name, ONE_SIDED_CONTEXT,
            reinterpret_cast<std::uintptr_t>(ctx1.get()));

        if (row_pivot_depth > -1) {
            ctx1->set_depth(row_pivot_depth - 1);
        } else {
            ctx1->set_depth(row_pivots.size());
        }

        return ctx1;
    }

    template <>
    std::shared_ptr<t_ctx2>
    make_context(std::shared_ptr<Table> table, const t_schema& schema,
        const t_view_config& view_config, std::string name) {
        bool column_only = view_config.is_column_only();
        auto row_pivots = view_config.get_row_pivots();
        auto column_pivots = view_config.get_column_pivots();
        auto aggspecs = view_config.get_aggspecs();
        auto filter_op = view_config.get_filter_op();
        auto fterm = view_config.get_fterm();
        auto sortspec = view_config.get_sortspec();
        auto col_sortspec = view_config.get_col_sortspec();
        auto row_pivot_depth = view_config.get_row_pivot_depth();
        auto column_pivot_depth = view_config.get_column_pivot_depth();

        t_totals total = sortspec.size() > 0 ? TOTALS_BEFORE : TOTALS_HIDDEN;

        auto cfg = t_config(
            row_pivots, column_pivots, aggspecs, total, filter_op, fterm, column_only);
        auto ctx2 = std::make_shared<t_ctx2>(schema, cfg);

        ctx2->init();

        auto pool = table->get_pool();
        auto gnode = table->get_gnode();
        pool->register_context(gnode->get_id(), name, TWO_SIDED_CONTEXT,
            reinterpret_cast<std::uintptr_t>(ctx2.get()));

        if (row_pivot_depth > -1) {
            ctx2->set_depth(t_header::HEADER_ROW, row_pivot_depth - 1);
        } else {
            ctx2->set_depth(t_header::HEADER_ROW, row_pivots.size());
        }

        if (column_pivot_depth > -1) {
            ctx2->set_depth(t_header::HEADER_COLUMN, column_pivot_depth - 1);
        } else {
            ctx2->set_depth(t_header::HEADER_COLUMN, column_pivots.size());
        }

        if (sortspec.size() > 0) {
            ctx2->sort_by(sortspec);
        }

        if (col_sortspec.size() > 0) {
            ctx2->column_sort_by(col_sortspec);
        }

        return ctx2;
    }

    /******************************************************************************
     *
     * Data serialization
     */

    template <>
    t_val
    get_column_data(std::shared_ptr<t_data_table> table, std::string colname) {
        t_val arr = t_val::array();
        auto col = table->get_column(colname);
        for (auto idx = 0; idx < col->size(); ++idx) {
            arr.set(idx, scalar_to_val(col->get_scalar(idx)));
        }
        return arr;
    }

    template <typename CTX_T>
    std::shared_ptr<t_data_slice<CTX_T>>
    get_data_slice(std::shared_ptr<View<CTX_T>> view, std::uint32_t start_row,
        std::uint32_t end_row, std::uint32_t start_col, std::uint32_t end_col) {
        auto data_slice = view->get_data(start_row, end_row, start_col, end_col);
        return data_slice;
    }

    template <typename CTX_T>
    t_val
    get_from_data_slice(
        std::shared_ptr<t_data_slice<CTX_T>> data_slice, t_uindex ridx, t_uindex cidx) {
        auto d = data_slice->get(ridx, cidx);
        return scalar_to_val(d);
    }

} // end namespace binding
} // end namespace perspective

using namespace perspective::binding;

/**
 * Main
 */
int
main(int argc, char** argv) {
    // clang-format off
EM_ASM({

    if (typeof self !== "undefined") {
        if (self.dispatchEvent && !self._perspective_initialized && self.document) {
            self._perspective_initialized = true;
            var event = self.document.createEvent("Event");
            event.initEvent("perspective-ready", false, true);
            self.dispatchEvent(event);
        } else if (!self.document && self.postMessage) {
            self.postMessage({});
        }
    }

});
    // clang-format on
}

/******************************************************************************
 *
 * Embind
 */

EMSCRIPTEN_BINDINGS(perspective) {
    /******************************************************************************
     *
     * Table
     */
    class_<Table>("Table")
        .constructor<std::shared_ptr<t_pool>, std::vector<std::string>, std::vector<t_dtype>,
            std::uint32_t, std::uint32_t, std::string, t_op, bool>()
        .smart_ptr<std::shared_ptr<Table>>("shared_ptr<Table>")
        .function("size", &Table::size)
        .function("get_schema", &Table::get_schema)
        .function("unregister_gnode", &Table::unregister_gnode)
        .function("reset_gnode", &Table::reset_gnode)
        .function("get_id", &Table::get_id)
        .function("get_pool", &Table::get_pool)
        .function("get_gnode", &Table::get_gnode);
    /******************************************************************************
     *
     * View
     */
    // Bind a View for each context type

    class_<View<t_ctx0>>("View_ctx0")
        .constructor<std::shared_ptr<Table>, std::shared_ptr<t_ctx0>, std::string, std::string,
            t_view_config>()
        .smart_ptr<std::shared_ptr<View<t_ctx0>>>("shared_ptr<View_ctx0>")
        .function("sides", &View<t_ctx0>::sides)
        .function("num_rows", &View<t_ctx0>::num_rows)
        .function("num_columns", &View<t_ctx0>::num_columns)
        .function("get_row_expanded", &View<t_ctx0>::get_row_expanded)
        .function("schema", &View<t_ctx0>::schema)
        .function("column_names", &View<t_ctx0>::column_names)
        .function("_get_deltas_enabled", &View<t_ctx0>::_get_deltas_enabled)
        .function("_set_deltas_enabled", &View<t_ctx0>::_set_deltas_enabled)
        .function("get_context", &View<t_ctx0>::get_context, allow_raw_pointers())
        .function("get_row_pivots", &View<t_ctx0>::get_row_pivots)
        .function("get_column_pivots", &View<t_ctx0>::get_column_pivots)
        .function("get_aggregates", &View<t_ctx0>::get_aggregates)
        .function("get_filter", &View<t_ctx0>::get_filter)
        .function("get_sort", &View<t_ctx0>::get_sort)
        .function("get_step_delta", &View<t_ctx0>::get_step_delta)
        .function("get_row_delta", &View<t_ctx0>::get_row_delta)
        .function("get_column_dtype", &View<t_ctx0>::get_column_dtype)
        .function("is_column_only", &View<t_ctx0>::is_column_only);

    class_<View<t_ctx1>>("View_ctx1")
        .constructor<std::shared_ptr<Table>, std::shared_ptr<t_ctx1>, std::string, std::string,
            t_view_config>()
        .smart_ptr<std::shared_ptr<View<t_ctx1>>>("shared_ptr<View_ctx1>")
        .function("sides", &View<t_ctx1>::sides)
        .function("num_rows", &View<t_ctx1>::num_rows)
        .function("num_columns", &View<t_ctx1>::num_columns)
        .function("get_row_expanded", &View<t_ctx1>::get_row_expanded)
        .function("expand", &View<t_ctx1>::expand)
        .function("collapse", &View<t_ctx1>::collapse)
        .function("set_depth", &View<t_ctx1>::set_depth)
        .function("schema", &View<t_ctx1>::schema)
        .function("column_names", &View<t_ctx1>::column_names)
        .function("_get_deltas_enabled", &View<t_ctx1>::_get_deltas_enabled)
        .function("_set_deltas_enabled", &View<t_ctx1>::_set_deltas_enabled)
        .function("get_context", &View<t_ctx1>::get_context, allow_raw_pointers())
        .function("get_row_pivots", &View<t_ctx1>::get_row_pivots)
        .function("get_column_pivots", &View<t_ctx1>::get_column_pivots)
        .function("get_aggregates", &View<t_ctx1>::get_aggregates)
        .function("get_filter", &View<t_ctx1>::get_filter)
        .function("get_sort", &View<t_ctx1>::get_sort)
        .function("get_step_delta", &View<t_ctx1>::get_step_delta)
        .function("get_row_delta", &View<t_ctx1>::get_row_delta)
        .function("get_column_dtype", &View<t_ctx1>::get_column_dtype)
        .function("is_column_only", &View<t_ctx1>::is_column_only);

    class_<View<t_ctx2>>("View_ctx2")
        .constructor<std::shared_ptr<Table>, std::shared_ptr<t_ctx2>, std::string, std::string,
            t_view_config>()
        .smart_ptr<std::shared_ptr<View<t_ctx2>>>("shared_ptr<View_ctx2>")
        .function("sides", &View<t_ctx2>::sides)
        .function("num_rows", &View<t_ctx2>::num_rows)
        .function("num_columns", &View<t_ctx2>::num_columns)
        .function("get_row_expanded", &View<t_ctx2>::get_row_expanded)
        .function("expand", &View<t_ctx2>::expand)
        .function("collapse", &View<t_ctx2>::collapse)
        .function("set_depth", &View<t_ctx2>::set_depth)
        .function("schema", &View<t_ctx2>::schema)
        .function("column_names", &View<t_ctx2>::column_names)
        .function("_get_deltas_enabled", &View<t_ctx2>::_get_deltas_enabled)
        .function("_set_deltas_enabled", &View<t_ctx2>::_set_deltas_enabled)
        .function("get_context", &View<t_ctx2>::get_context, allow_raw_pointers())
        .function("get_row_pivots", &View<t_ctx2>::get_row_pivots)
        .function("get_column_pivots", &View<t_ctx2>::get_column_pivots)
        .function("get_aggregates", &View<t_ctx2>::get_aggregates)
        .function("get_filter", &View<t_ctx2>::get_filter)
        .function("get_sort", &View<t_ctx2>::get_sort)
        .function("get_row_path", &View<t_ctx2>::get_row_path)
        .function("get_step_delta", &View<t_ctx2>::get_step_delta)
        .function("get_row_delta", &View<t_ctx2>::get_row_delta)
        .function("get_column_dtype", &View<t_ctx2>::get_column_dtype)
        .function("is_column_only", &View<t_ctx2>::is_column_only);

    /******************************************************************************
     *
     * t_view_config
     */
    class_<t_view_config>("t_view_config")
        .constructor<std::vector<std::string>, std::vector<std::string>,
            tsl::ordered_map<std::string, std::string>, std::vector<std::string>,
            std::vector<std::tuple<std::string, std::string, std::vector<t_tscalar>>>,
            std::vector<std::vector<std::string>>, std::string, bool>()
        .function("add_filter_term", &t_view_config::add_filter_term);

    /******************************************************************************
     *
     * t_data_table
     */
    class_<t_data_table>("t_data_table")
        .smart_ptr<std::shared_ptr<t_data_table>>("shared_ptr<t_data_table>")
        .function<unsigned long>("size",
            reinterpret_cast<unsigned long (t_data_table::*)() const>(&t_data_table::size));

    /******************************************************************************
     *
     * t_schema
     */
    class_<t_schema>("t_schema")
        .function<const std::vector<std::string>&>(
            "columns", &t_schema::columns, allow_raw_pointers())
        .function<const std::vector<t_dtype>>("types", &t_schema::types, allow_raw_pointers());

    /******************************************************************************
     *
     * t_gnode
     */
    class_<t_gnode>("t_gnode")
        .smart_ptr<std::shared_ptr<t_gnode>>("shared_ptr<t_gnode>")
        .function<t_uindex>(
            "get_id", reinterpret_cast<t_uindex (t_gnode::*)() const>(&t_gnode::get_id))
        .function<t_schema>("get_tblschema", &t_gnode::get_tblschema)
        .function<void>("reset", &t_gnode::reset)
        .function<t_data_table*>("get_table", &t_gnode::get_table, allow_raw_pointers());

    /******************************************************************************
     *
     * t_data_slice
     */
    class_<t_data_slice<t_ctx0>>("t_data_slice_ctx0")
        .smart_ptr<std::shared_ptr<t_data_slice<t_ctx0>>>("shared_ptr<t_data_slice<t_ctx0>>>")
        .function<std::vector<t_tscalar>>(
            "get_column_slice", &t_data_slice<t_ctx0>::get_column_slice)
        .function<const std::vector<t_tscalar>&>("get_slice", &t_data_slice<t_ctx0>::get_slice)
        .function<const std::vector<std::vector<t_tscalar>>&>(
            "get_column_names", &t_data_slice<t_ctx0>::get_column_names);

    class_<t_data_slice<t_ctx1>>("t_data_slice_ctx1")
        .smart_ptr<std::shared_ptr<t_data_slice<t_ctx1>>>("shared_ptr<t_data_slice<t_ctx1>>>")
        .function<std::vector<t_tscalar>>(
            "get_column_slice", &t_data_slice<t_ctx1>::get_column_slice)
        .function<const std::vector<t_tscalar>&>("get_slice", &t_data_slice<t_ctx1>::get_slice)
        .function<const std::vector<std::vector<t_tscalar>>&>(
            "get_column_names", &t_data_slice<t_ctx1>::get_column_names)
        .function<std::vector<t_tscalar>>("get_row_path", &t_data_slice<t_ctx1>::get_row_path);

    class_<t_data_slice<t_ctx2>>("t_data_slice_ctx2")
        .smart_ptr<std::shared_ptr<t_data_slice<t_ctx2>>>("shared_ptr<t_data_slice<t_ctx2>>>")
        .function<std::vector<t_tscalar>>(
            "get_column_slice", &t_data_slice<t_ctx2>::get_column_slice)
        .function<const std::vector<t_tscalar>&>("get_slice", &t_data_slice<t_ctx2>::get_slice)
        .function<const std::vector<std::vector<t_tscalar>>&>(
            "get_column_names", &t_data_slice<t_ctx2>::get_column_names)
        .function<std::vector<t_tscalar>>("get_row_path", &t_data_slice<t_ctx2>::get_row_path);

    /******************************************************************************
     *
     * t_ctx0
     */
    class_<t_ctx0>("t_ctx0").smart_ptr<std::shared_ptr<t_ctx0>>("shared_ptr<t_ctx0>");

    /******************************************************************************
     *
     * t_ctx1
     */
    class_<t_ctx1>("t_ctx1").smart_ptr<std::shared_ptr<t_ctx1>>("shared_ptr<t_ctx1>");

    /******************************************************************************
     *
     * t_ctx2
     */
    class_<t_ctx2>("t_ctx2").smart_ptr<std::shared_ptr<t_ctx2>>("shared_ptr<t_ctx2>");

    /******************************************************************************
     *
     * t_pool
     */
    class_<t_pool>("t_pool")
        .constructor<>()
        .smart_ptr<std::shared_ptr<t_pool>>("shared_ptr<t_pool>")
        .function<void>("unregister_gnode", &t_pool::unregister_gnode)
        .function<void>("_process", &t_pool::_process)
        .function<void>("set_update_delegate", &t_pool::set_update_delegate);

    /******************************************************************************
     *
     * t_tscalar
     */
    class_<t_tscalar>("t_tscalar");

    /******************************************************************************
     *
     * t_updctx
     */
    value_object<t_updctx>("t_updctx")
        .field("gnode_id", &t_updctx::m_gnode_id)
        .field("ctx_name", &t_updctx::m_ctx);

    /******************************************************************************
     *
     * t_cellupd
     */
    value_object<t_cellupd>("t_cellupd")
        .field("row", &t_cellupd::row)
        .field("column", &t_cellupd::column)
        .field("old_value", &t_cellupd::old_value)
        .field("new_value", &t_cellupd::new_value);

    /******************************************************************************
     *
     * t_stepdelta
     */
    value_object<t_stepdelta>("t_stepdelta")
        .field("rows_changed", &t_stepdelta::rows_changed)
        .field("columns_changed", &t_stepdelta::columns_changed)
        .field("cells", &t_stepdelta::cells);

    /******************************************************************************
     *
     * vector
     */
    register_vector<std::int32_t>("std::vector<std::int32_t>");
    register_vector<std::string>("std::vector<std::string>");
    register_vector<t_dtype>("std::vector<t_dtype>");
    register_vector<t_cellupd>("std::vector<t_cellupd>");
    register_vector<t_tscalar>("std::vector<t_tscalar>");
    register_vector<t_updctx>("std::vector<t_updctx>");
    register_vector<t_uindex>("std::vector<t_uindex>");
    register_vector<t_val>("std::vector<t_val>");
    register_vector<std::vector<t_tscalar>>("std::vector<std::vector<t_tscalar>>");
    register_vector<std::vector<std::string>>("std::vector<std::vector<std::string>>");
    register_vector<std::vector<t_val>>("std::vector<std::vector<t_val>>");

    /******************************************************************************
     *
     * map
     */
    register_map<std::string, std::string>("std::map<std::string, std::string>");

    /******************************************************************************
     *
     * t_dtype
     */
    enum_<t_dtype>("t_dtype")
        .value("DTYPE_NONE", DTYPE_NONE)
        .value("DTYPE_INT64", DTYPE_INT64)
        .value("DTYPE_INT32", DTYPE_INT32)
        .value("DTYPE_INT16", DTYPE_INT16)
        .value("DTYPE_INT8", DTYPE_INT8)
        .value("DTYPE_UINT64", DTYPE_UINT64)
        .value("DTYPE_UINT32", DTYPE_UINT32)
        .value("DTYPE_UINT16", DTYPE_UINT16)
        .value("DTYPE_UINT8", DTYPE_UINT8)
        .value("DTYPE_FLOAT64", DTYPE_FLOAT64)
        .value("DTYPE_FLOAT32", DTYPE_FLOAT32)
        .value("DTYPE_BOOL", DTYPE_BOOL)
        .value("DTYPE_TIME", DTYPE_TIME)
        .value("DTYPE_DATE", DTYPE_DATE)
        .value("DTYPE_ENUM", DTYPE_ENUM)
        .value("DTYPE_OID", DTYPE_OID)
        .value("DTYPE_PTR", DTYPE_PTR)
        .value("DTYPE_F64PAIR", DTYPE_F64PAIR)
        .value("DTYPE_USER_FIXED", DTYPE_USER_FIXED)
        .value("DTYPE_STR", DTYPE_STR)
        .value("DTYPE_USER_VLEN", DTYPE_USER_VLEN)
        .value("DTYPE_LAST_VLEN", DTYPE_LAST_VLEN)
        .value("DTYPE_LAST", DTYPE_LAST);

    /******************************************************************************
     *
     * t_op
     */
    enum_<t_op>("t_op")
        .value("OP_INSERT", OP_INSERT)
        .value("OP_DELETE", OP_DELETE)
        .value("OP_CLEAR", OP_CLEAR)
        .value("OP_UPDATE", OP_UPDATE);

    /******************************************************************************
     *
     * Construct `std::vector`s
     */
    function("make_string_vector", &make_vector<std::string>);
    function("make_val_vector", &make_vector<t_val>);
    function("make_2d_string_vector", &make_vector<std::vector<std::string>>);
    function("make_2d_val_vector", &make_vector<std::vector<t_val>>);

    /******************************************************************************
     *
     * Perspective functions
     */
    function("make_table", &make_table<t_val>);
    function("make_computed_table", &make_computed_table<t_val>);
    function("scalar_vec_to_val", &scalar_vec_to_val);
    function("scalar_vec_to_string", &scalar_vec_to_string);
    function("table_add_computed_column", &table_add_computed_column<t_val>);
    function("col_to_js_typed_array", &col_to_js_typed_array);
    function("make_view_zero", &make_view<t_ctx0>);
    function("make_view_one", &make_view<t_ctx1>);
    function("make_view_two", &make_view<t_ctx2>);
    function("get_data_slice_zero", &get_data_slice<t_ctx0>, allow_raw_pointers());
    function("get_from_data_slice_zero", &get_from_data_slice<t_ctx0>, allow_raw_pointers());
    function("get_data_slice_one", &get_data_slice<t_ctx1>, allow_raw_pointers());
    function("get_from_data_slice_one", &get_from_data_slice<t_ctx1>, allow_raw_pointers());
    function("get_data_slice_two", &get_data_slice<t_ctx2>, allow_raw_pointers());
    function("get_from_data_slice_two", &get_from_data_slice<t_ctx2>, allow_raw_pointers());
}