#include "Check.hpp"
#include <Core/IO/Logger.hpp>

namespace graphics::vulkan {
namespace {
    bool decodeVkResult(VkResult result, std::string_view& description) {
#define ON_SUCCESS_CODE(code, desc) case code: description = #code ": " desc; return true
#define ON_ERROR_CODE(code, desc)   case code: description = #code ": " desc; return false
        switch (result) {
            ON_SUCCESS_CODE(VK_SUCCESS,                                         "Command successfully completed");
            ON_SUCCESS_CODE(VK_NOT_READY,                                       "A fence or query has not yet completed");
            ON_SUCCESS_CODE(VK_TIMEOUT,                                         "A wait operation has not completed in the specified time");
            ON_SUCCESS_CODE(VK_EVENT_SET,                                       "An event is signaled");
            ON_SUCCESS_CODE(VK_EVENT_RESET,                                     "An event is unsignaled");
            ON_SUCCESS_CODE(VK_INCOMPLETE,                                      "A return array was too small for the result");
            ON_SUCCESS_CODE(VK_SUBOPTIMAL_KHR,                                  "A swapchain no longer matches the surface properties exactly, but can still be used to present to the surface successfully");
            ON_SUCCESS_CODE(VK_THREAD_IDLE_KHR,                                 "A deferred operation is not complete but there is currently no work for this thread to do at the time of this call");
            ON_SUCCESS_CODE(VK_THREAD_DONE_KHR,                                 "A deferred operation is not complete but there is no work remaining to assign to additional threads");
            ON_SUCCESS_CODE(VK_OPERATION_DEFERRED_KHR,                          "A deferred operation was requested and at least some of the work was deferred");
            ON_SUCCESS_CODE(VK_OPERATION_NOT_DEFERRED_KHR,                      "A deferred operation was requested and no operations were deferred");
            ON_SUCCESS_CODE(VK_PIPELINE_COMPILE_REQUIRED,                       "A requested pipeline creation would have required compilation, but the application requested compilation to not be performed");
            ON_SUCCESS_CODE(VK_PIPELINE_BINARY_MISSING_KHR,                     "The application attempted to create a pipeline binary by querying an internal cache, but the internal cache entry did not exist");
            ON_SUCCESS_CODE(VK_INCOMPATIBLE_SHADER_BINARY_EXT,                  "The provided binary shader code is not compatible with this device");
            ON_ERROR_CODE(  VK_ERROR_OUT_OF_HOST_MEMORY,                        "A host memory allocation has failed");
            ON_ERROR_CODE(  VK_ERROR_OUT_OF_DEVICE_MEMORY,                      "A device memory allocation has failed");
            ON_ERROR_CODE(  VK_ERROR_INITIALIZATION_FAILED,                     "Initialization of an object could not be completed for implementation-specific reasons");
            ON_ERROR_CODE(  VK_ERROR_DEVICE_LOST,                               "The logical or physical device has been lost");
            ON_ERROR_CODE(  VK_ERROR_MEMORY_MAP_FAILED,                         "Mapping of a memory object has failed");
            ON_ERROR_CODE(  VK_ERROR_LAYER_NOT_PRESENT,                         "A requested layer is not present or could not be loaded");
            ON_ERROR_CODE(  VK_ERROR_EXTENSION_NOT_PRESENT,                     "A requested extension is not supported");
            ON_ERROR_CODE(  VK_ERROR_FEATURE_NOT_PRESENT,                       "A requested feature is not supported");
            ON_ERROR_CODE(  VK_ERROR_INCOMPATIBLE_DRIVER,                       "The requested version of Vulkan is not supported by the driver or is otherwise incompatible for implementation-specific reasons");
            ON_ERROR_CODE(  VK_ERROR_TOO_MANY_OBJECTS,                          "Too many objects of the type have already been created");
            ON_ERROR_CODE(  VK_ERROR_FORMAT_NOT_SUPPORTED,                      "A requested format is not supported on this device");
            ON_ERROR_CODE(  VK_ERROR_FRAGMENTED_POOL,                           "A pool allocation has failed due to fragmentation of the pool’s memory");
            ON_ERROR_CODE(  VK_ERROR_SURFACE_LOST_KHR,                          "A surface is no longer available");
            ON_ERROR_CODE(  VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,                  "The requested window is already in use by Vulkan or another API in a manner which prevents it from being used again");
            ON_ERROR_CODE(  VK_ERROR_OUT_OF_DATE_KHR,                           "A surface has changed in such a way that it is no longer compatible with the swapchain, and further presentation requests using the swapchain will fail");
            ON_ERROR_CODE(  VK_ERROR_INCOMPATIBLE_DISPLAY_KHR,                  "The display used by a swapchain does not use the same presentable image layout, or is incompatible in a way that prevents sharing an image");
            ON_ERROR_CODE(  VK_ERROR_INVALID_SHADER_NV,                         "One or more shaders failed to compile or link. More details are reported back to the application via VK_EXT_debug_report if enabled");
            ON_ERROR_CODE(  VK_ERROR_OUT_OF_POOL_MEMORY,                        "A pool memory allocation has failed");
            ON_ERROR_CODE(  VK_ERROR_INVALID_EXTERNAL_HANDLE,                   "An external handle is not a valid handle of the specified type");
            ON_ERROR_CODE(  VK_ERROR_FRAGMENTATION,                             "A descriptor pool creation has failed due to fragmentation");
            // ON_ERROR_CODE(  VK_ERROR_INVALID_DEVICE_ADDRESS_EXT,             "A buffer creation failed because the requested address is not available");
            ON_ERROR_CODE(  VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,            "A buffer creation or memory allocation failed because the requested address is not available. A shader group handle assignment failed because the requested shader group handle information is no longer valid.");
            ON_ERROR_CODE(  VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT,       "An operation on a swapchain created with VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT failed as it did not have exclusive full-screen access. This may occur due to implementation-dependent reasons, outside of the application’s control.");
            ON_ERROR_CODE(  VK_ERROR_VALIDATION_FAILED_EXT,                     "A command failed because invalid usage was detected by the implementation or a validation-layer");
            ON_ERROR_CODE(  VK_ERROR_COMPRESSION_EXHAUSTED_EXT,                 "An image creation failed because internal resources required for compression are exhausted. This must only be returned when fixed-rate compression is requested.");
            ON_ERROR_CODE(  VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR,             "The requested VkImageUsageFlags are not supported");
            ON_ERROR_CODE(  VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR,    "The requested video picture layout is not supported");
            ON_ERROR_CODE(  VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR, "A video profile operation specified via VkVideoProfileInfoKHR::videoCodecOperation is not supported");
            ON_ERROR_CODE(  VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR,    "Format parameters in a requested VkVideoProfileInfoKHR chain are not supported");
            ON_ERROR_CODE(  VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR,     "Codec-specific parameters in a requested VkVideoProfileInfoKHR chain are not supported");
            ON_ERROR_CODE(  VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR,       "The specified video Std header version is not supported");
            ON_ERROR_CODE(  VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR,          "The specified Video Std parameters do not adhere to the syntactic or semantic requirements of the used video compression standard, or values derived from parameters according to the rules defined by the used video compression standard do not adhere to the capabilities of the video compression standard or the implementation.");
            ON_ERROR_CODE(  VK_ERROR_NOT_PERMITTED,                             "The driver implementation has denied a request to acquire a priority above the default priority (VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT) because the application does not have sufficient privileges.");
            ON_ERROR_CODE(  VK_ERROR_NOT_ENOUGH_SPACE_KHR,                      "The application did not provide enough space to return all the required data");
            ON_ERROR_CODE(  VK_ERROR_UNKNOWN,                                   "An unknown error has occurred; either the application has provided invalid input, or an implementation failure has occurred.");
        default:
            core::io::error("Unknown VkResult value: {}", static_cast<int64_t>(result));
            description = "???";
            return false;
        }
#undef ON_SUCCESS_CODE
#undef ON_ERROR_CODE
    }
} // namespace

    bool checkVkResult(VkResult result, const std::source_location& loc) {
        if (result == VK_SUCCESS)
            return true;
        std::string_view description;
        bool const isSuccess = decodeVkResult(result, description);
        if (isSuccess) {
            core::io::trace("Operation returned {}\nat {}:{}:{}, function {}", description, loc.file_name(), loc.line(), loc.column(), loc.function_name());
        } else {
            core::io::error("Vulkan error: {}\nat {}:{}:{}, function {}", description, loc.file_name(), loc.line(), loc.column(), loc.function_name());
        }
        return isSuccess;
    }
} // namespace graphics::vulkan
