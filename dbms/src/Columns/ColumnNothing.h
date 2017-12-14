#pragma once

#include <Columns/IColumnDummy.h>


namespace DB
{

class ColumnNothing final : public IColumnDummy
{
public:
    using IColumnDummy::IColumnDummy;

    const char * getFamilyName() const override { return "Nothing"; }
    ColumnPtr cloneDummy(size_t s) const override { return ColumnNothing::create(s); };

    bool canBeInsideNullable() const override { return true; }
};

}
