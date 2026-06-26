# In header

In order to declare new Vulkan resource, you must first include header <core/common/Resource.hpp>.

Then you have to declare the new class for the raw resource management and inherit it from `core::VulkanResourceBase<{HandleType}>`:

```cpp
class RawResource : public core::VulkanResourceBase<VkResource> { ... };
```

Then you need to add the metadata macros to the beginning of the class body. The final header form is thus:

```cpp
class RawResource : public core::VulkanResourceBase<VkResource> {
    // List the context needed to destroy the resource if any.
    CORE_VK_RESOURCE_CONTEXT(RawResource, DestructionContext1 ctx1; ...)
    CORE_VK_RESOURCE_CONSTRUCTION_FROM(OtherResource1 res1, OtherResource2 const& res2, ...) {
        // Initialize self fields (including self.m_handle which has VkResource type).

        // Then you need to provide the context that will later be needed to destroy the resource.
        CORE_VK_CAPTURE_DESTRUCTION_CONTEXT() {
            .ctx1 = ...,
        };
    }
public:
    // List here all the additional methods you need.
private:
    // ... And fields.
};
```

Instead of CORE_VKRESOURCE_CONSTRUCTION_FROM alternatively you can use

```cpp
CORE_VK_RESOURCE_DEFER_CONSTRUCTION_FROM(OtherResource1 res1, OtherResource2 const& res2, ...);
```

To define it elsewhere.

## Wrappers

After class declaration provide a RAII wrapper over the resource and use it.

```cpp
using Resource = VulkanRaii<RawResource>;
using Resources = VulkanRaiiVector<RawResource>; // If it makes sense
```

It will have all the same methods as your class, the constructor, and additional methods:
1. raw() to access RawResource.
2. grabRaw() to grab RawResource and own it externally.
3. destroyer() to access the destroyer that will have the context you declared as its fields.
4. handle() to access the underlying handle (note that RawResource will also have it).
5. isNull() to check if there is any resource inside (note that RawResource will also have it).

TODO: implement VulkanRaiiVector (probably need to add `CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM`).

As for Resources, note that here destroyer is optional. In some cases you will have to manually provide it by `setDestroyer()`, e.g. in cases like:

```cpp
SomeResources resources(16);
vkCreateThisResourcesInBatch(resources.dataAsHandles(), resources.size());
// Now resources exist, but destroyer is still nullopt
```

You can read more about available methods in [Resource.hpp](src/core/include/core/vulkan/Resource.hpp).

# In source file

You have to declare

```cpp
CORE_VK_RESOURCE_DESTROY_IMPL(RawResource) {
    // Destroy RawResource self here.
    // This will contain the destruction context.
}
```

Note that it is guaranteed that here self.isNull() is false - no need to check for it. Also no need to reset the resource - it will be done automatically.

If you deferred construction definition, you must additionally define it:

```cpp
CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawResource, same arguments as in CORE_VK_RESOURCE_DEFER_CONSTRUCTION_FROM) {
    // Same body as for CORE_VKRESOURCE_CONSTRUCTION_FROM
}
```

# What it all unfolds to

TODO: fill in
