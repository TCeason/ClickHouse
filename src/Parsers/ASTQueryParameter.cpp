#include <Parsers/ASTQueryParameter.h>
#include <IO/WriteHelpers.h>
#include <Common/quoteString.h>
#include <IO/Operators.h>


namespace DB
{

void ASTQueryParameter::formatImplWithoutAlias(WriteBuffer & ostr, const FormatSettings &, FormatState &, FormatStateStacked) const
{
    ostr << '{' << name << ':' << type << '}';
}

ASTPtr ASTQueryParameter::clone() const
{
    auto ret = std::make_shared<ASTQueryParameter>(*this);
    ret->cloneChildren();
    return ret;
}

void ASTQueryParameter::appendColumnNameImpl(WriteBuffer & ostr) const
{
    writeString(name, ostr);
}

void ASTQueryParameter::updateTreeHashImpl(SipHash & hash_state, bool ignore_aliases) const
{
    ASTWithAlias::updateTreeHashImpl(hash_state, ignore_aliases);
}

}
