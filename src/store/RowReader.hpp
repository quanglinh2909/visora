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
//   * cast the column in SQL (`id::text`), so the type is what the reader
//     expects rather than what the driver chose;
//   * read it through here, so a column that still surprises us produces a
//     default value and a warning rather than a core dump.
//
// The same trap as the configuration loader, which is why the helper is shaped
// the same way.

#include <string>

#include "oatpp/core/Types.hpp"

#include "core/Log.hpp"

namespace visora::store {

template <class Wrapper>
bool tryRetrieve(const oatpp::Any& cell, Wrapper& out) {
    if (!cell) return false;
    try {
        out = cell.retrieve<Wrapper>();
        return static_cast<bool>(out);
    } catch (const std::runtime_error& error) {
        // Names the column type mismatch rather than dying on it. A row that
        // reads mostly right is far better than a server that will not stay up.
        VS_WARN("store") << "unexpected column type: " << error.what();
        return false;
    }
}

// One reader per row, indexed by position — which is why the column list and
// the reader have to be written next to each other.
class RowReader {
public:
    explicit RowReader(const oatpp::Vector<oatpp::Any>& row) : m_row(row) {}

    std::string str(std::size_t index) const {
        oatpp::String value;
        if (!at(index, value)) return {};
        return *value;
    }

    bool boolean(std::size_t index) const {
        oatpp::Boolean value;
        return at(index, value) && *value;
    }

    int integer(std::size_t index) const {
        oatpp::Int32 value;
        if (at(index, value)) return *value;
        // PostgreSQL widens an expression to bigint readily enough that asking
        // only for Int32 loses columns that are perfectly valid integers.
        oatpp::Int64 wide;
        if (at(index, wide)) return static_cast<int>(*wide);
        return 0;
    }

    std::int64_t bigint(std::size_t index) const {
        oatpp::Int64 value;
        if (at(index, value)) return *value;
        oatpp::Int32 narrow;
        if (at(index, narrow)) return *narrow;
        return 0;
    }

    double real(std::size_t index) const {
        oatpp::Float64 value;
        if (at(index, value)) return *value;
        oatpp::Float32 narrow;
        if (at(index, narrow)) return *narrow;
        return 0.0;
    }

private:
    template <class Wrapper>
    bool at(std::size_t index, Wrapper& out) const {
        if (!m_row || index >= m_row->size()) return false;
        return tryRetrieve(m_row[index], out);
    }

    const oatpp::Vector<oatpp::Any>& m_row;
};

}  // namespace visora::store
