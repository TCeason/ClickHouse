#include <Functions/FunctionBase64Conversion.h>

#if USE_BASE64
#include <Functions/FunctionFactory.h>

namespace DB
{
namespace
{
struct NameBase64Decode
{
static constexpr auto name = "tryBase64Decode";
};

using Base64DecodeImpl = BaseXXDecode<Base64DecodeTraits<Base64Variant::Normal>, NameBase64Decode, BaseXXDecodeErrorHandling::ReturnEmptyString>;
using FunctionBase64Decode = FunctionBaseXXConversion<Base64DecodeImpl>;
}
REGISTER_FUNCTION(TryBase64Decode)
{
    FunctionDocumentation::Description description = R"(Decodes a String or FixedString from base64, like base64Decode but returns an empty string in case of an error.)";
    FunctionDocumentation::Syntax syntax = "tryBase64Decode(encoded)";
    FunctionDocumentation::Arguments arguments = {{"encoded", "String column or constant. If the string is not a valid Base64-encoded value, returns an empty string.", {"String"}}};
    FunctionDocumentation::ReturnedValue returned_value = {"Returns a string containing the decoded value of the argument.", {"String"}};
    FunctionDocumentation::Examples examples = {{"valid", "SELECT tryBase64Decode('Y2xpY2tob3VzZQ==')", "clickhouse"}, {"invalid", "SELECT tryBase64Decode('invalid')", ""}};
    FunctionDocumentation::IntroducedIn introduced_in = {18, 16};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Encoding;

    factory.registerFunction<FunctionBase64Decode>({description, syntax, arguments, returned_value, examples, introduced_in, category});
}
}

#endif
