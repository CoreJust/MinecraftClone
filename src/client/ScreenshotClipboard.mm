#import <AppKit/AppKit.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

NSPasteboard* pasteboardForName(char const* const name)
{
    if (name == nullptr) {
        return [NSPasteboard generalPasteboard];
    }
    NSString* const pasteboard_name = [NSString stringWithUTF8String:name];
    if (pasteboard_name == nil || pasteboard_name.length == 0U) {
        return nil;
    }
    return [NSPasteboard pasteboardWithName:pasteboard_name];
}

} // namespace

extern "C" bool mcCopyPngToMacClipboard(
    uint8_t const* const bytes,
    size_t const size,
    char const* const pasteboard_name
) noexcept
{
    if (bytes == nullptr || size == 0U) {
        return false;
    }
    @autoreleasepool {
        NSData* const data = [NSData dataWithBytes:bytes length:static_cast<NSUInteger>(size)];
        if (data == nil) {
            return false;
        }
        NSPasteboard* const pasteboard = pasteboardForName(pasteboard_name);
        if (pasteboard == nil) {
            return false;
        }
        [pasteboard clearContents];
        return [pasteboard setData:data forType:NSPasteboardTypePNG];
    }
}

namespace client::detail {

std::optional<std::vector<uint8_t>> takeScreenshotPngFromPasteboard(
    std::string_view const pasteboard_name,
    std::string& error
)
{
    if (pasteboard_name.empty()) {
        error = "pasteboard name is empty";
        return std::nullopt;
    }
    std::string const name_storage{ pasteboard_name };
    @autoreleasepool {
        NSPasteboard* const pasteboard = pasteboardForName(name_storage.c_str());
        if (pasteboard == nil) {
            error = "could not open the named macOS pasteboard";
            return std::nullopt;
        }
        NSData* const data = [pasteboard dataForType:NSPasteboardTypePNG];
        if (data == nil || data.length == 0U) {
            [pasteboard releaseGlobally];
            error = "named macOS pasteboard does not contain PNG data";
            return std::nullopt;
        }
        uint8_t const* const bytes = static_cast<uint8_t const*>(data.bytes);
        std::vector<uint8_t> png{ bytes, bytes + data.length };
        [pasteboard releaseGlobally];
        error.clear();
        return png;
    }
}

} // namespace client::detail
