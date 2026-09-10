#pragma once

// Reading a column out of an oatpp query result without ending the process.
//
// oatpp::Any::retrieve<T>() THROWS when the stored type is not T, and the throw
// happens on an HTTP worker thread with no handler above it — so one column
// whose type is not what the reader expected aborts the whole server. That is
// not a hypothetical: the camera repository crashed on its first real query
// because PostgreSQL hands a UUID column back as a UUID, not as a String.
//
// Two defences, and both are needed:
//
//   * cast the column in SQL (`CAST(id AS TEXT)`), so the type is what the
//     reader expects rather than what the driver chose;
//   * read it through here, so a column that still surprises us produces a
//     default value and a warning rather than a core dump.
//
// A NUMERIC COLUMN IS READ THROUGH SEVERAL TYPES, in width order, and only a
// value that matches none of them is a problem. A schema in the field is not
// the schema in sql/schema.sql: a table created years ago by an earlier version
// has `smallint` where this one says `integer`, and PostgreSQL widens an
// expression to `bigint` readily. All three are perfectly good integers.
//
// THE WARNING NAMES THE COLUMN. It used to say only "unexpected column type",
// once per attempted type — so a column that read fine on the second attempt
// still logged, and a column that read as nothing at all did not say which one.
// A board reported 204 of those and none of them said that `motion_grid_x` was
// coming back as 0, which is a motion grid with no cells in it.

#include <cstdint>
#include <string>

#include "oatpp/core/Types.hpp"

#include "core/Log.hpp"

namespace visora::store {

enum class CellRead {
    Ok,
    Null,      // the column is NULL, which is a value and not a problem
    Mismatch,  // the stored type is not the one asked for
};

template <class Wrapper>
CellRead tryRetrieve(const oatpp::Any& cell, Wrapper& out) {
    if (!cell) return CellRead::Null;
    try {
        out = cell.retrieve<Wrapper>();
        return out ? CellRead::Ok : CellRead::Null;
    } catch (const std::runtime_error&) {
        return CellRead::Mismatch;
    }
}

// One reader per row, indexed by position — which is why the column list and
// the reader have to be written next to each other.
class RowReader {
public:
    explicit RowReader(const oatpp::Vector<oatpp::Any>& row) : m_row(row) {}

    std::string str(std::size_t index) const {
        oatpp::String value;
        if (at(index, value) != CellRead::Ok) {
            complainIfMismatch(index, "text");
            return {};
        }
        return *value;
    }

    bool boolean(std::size_t index) const {
        oatpp::Boolean value;
        if (at(index, value) != CellRead::Ok) {
            complainIfMismatch(index, "boolean");
            return false;
        }
        return *value;
    }

    int integer(std::size_t index) const {
        return static_cast<int>(anyInteger(index, "integer"));
    }

    std::int64_t bigint(std::size_t index) const { return anyInteger(index, "bigint"); }

    double real(std::size_t index) const {
        oatpp::Float64 wide;
        if (at(index, wide) == CellRead::Ok) return *wide;
        oatpp::Float32 narrow;
        if (at(index, narrow) == CellRead::Ok) return *narrow;
        complainIfMismatch(index, "number");
        return 0.0;
    }

private:
    // smallint, integer and bigint are all integers, and which one a column has
    // depends on when the table was created rather than on what the value
    // means. Asking for each in turn is the difference between reading a live
    // deployment's settings and quietly replacing them with zero.
    std::int64_t anyInteger(std::size_t index, const char* wanted) const {
        oatpp::Int32 value;
        if (at(index, value) == CellRead::Ok) return *value;
        oatpp::Int64 wide;
        if (at(index, wide) == CellRead::Ok) return *wide;
        oatpp::Int16 narrow;
        if (at(index, narrow) == CellRead::Ok) return *narrow;
        oatpp::Int8 tiny;
        if (at(index, tiny) == CellRead::Ok) return *tiny;
        complainIfMismatch(index, wanted);
        return 0;
    }

    template <class Wrapper>
    CellRead at(std::size_t index, Wrapper& out) const {
        if (!m_row || index >= m_row->size()) return CellRead::Null;
        return tryRetrieve(m_row[index], out);
    }

    // Only a cell that is present and read as nothing is worth a word. A NULL
    // column is a value; the default stands for it and always did.
    void complainIfMismatch(std::size_t index, const char* wanted) const {
        if (!m_row || index >= m_row->size() || !m_row[index]) return;
        VS_WARN("store") << "column " << index << " is not readable as " << wanted
                         << "; using the default";
    }

    const oatpp::Vector<oatpp::Any>& m_row;
};

}  // namespace visora::store
