# In header

To declare a Vulkan resource, include `<core/vulkan/Resource.hpp>`.

Declare the raw resource in `core::vk` and inherit `VulkanResourceBase<{HandleType}>`:

```cpp
class RawResource : public VulkanResourceBase<VkResource> { ... };
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
    // Batch construction is optional, but required to use VulkanRaiiVector. CORE_VK_RESOURCE_CONSTRUCTION_FROM will be inferred from it.
    CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM(OtherResource1 res1, OtherResource2 const& res2, ...) {
        // Initialize selves fields (including self.m_handle which has VkResource type).
        // selves is a span of Self type.

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

Alternatively you can use

```cpp
CORE_VK_RESOURCE_CONSTRUCTION_FROM(OtherResource1 res1, OtherResource2 const& res2, ...);
```

To define it elsewhere.

Same for `CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM`.

## Wrappers

After class declaration provide a RAII wrapper over the resource and use it.

```cpp
using Resource = VulkanRaii<RawResource>;
using Resources = VulkanRaiiVector<RawResource>;
```

Resource will have all the same methods as your class, the constructor, and additional methods:
1. raw() to access RawResource.
2. grabRaw() to grab RawResource and own it externally.
3. destroyer() to access the destroyer that will have the context you declared as its fields.
4. handle() to access the underlying handle (note that RawResource will also have it).
5. isNull() to check if there is any resource inside (note that RawResource will also have it).

Resources will be accessible via `operator[]`. To use Resources you must provide `CORE_VK_RESOURCE_BATCH_CONSTRUCTION_FROM` and add `CORE_VK_BATCH_DESTROYABLE` to `CORE_VK_RESOURCE_CONTEXT`.

See [`Resource.hpp`](../../../src/core/include/core/vulkan/Resource.hpp) and
[the Vulkan subsystem guide](../VULKAN.md) for the ownership model.

# In source file

You have to declare

```cpp
CORE_VK_RESOURCE_DESTROY_IMPL(RawResource) {
    // Destroy RawResource self here.
    // This will contain the destruction context.
}
```

Same for `CORE_VK_RESOURCE_BATCH_DESTROY_IMPL` if you declared `CORE_VK_BATCH_DESTROYABLE`.

Note that it is guaranteed that here self.isNull() is false - no need to check for it. Also no need to reset the resource - it will be done automatically.

If you deferred construction definition, you must additionally define it:

```cpp
CORE_VK_RESOURCE_DEFERRED_CONSTRUCTION_IMPL(RawResource, same arguments as in CORE_VK_RESOURCE_CONSTRUCTION_FROM) {
    // Same body as for CORE_VK_RESOURCE_CONSTRUCTION_FROM
}
```

Same for `CORE_VK_RESOURCE_DEFERRED_BATCH_CONSTRUCTION_IMPL`.

# Ownership contract behind the macros

`CORE_VK_RESOURCE_CONTEXT` declares a trivially copyable `Destroyer` carrying
only the parent handles/context needed at destruction. The construction macro
creates `make`/`makeWithDestroyer`; the latter pairs the raw handle with that
captured destroyer. `VulkanRaii<RawResource>` stores the pair, is move-only, and
calls the destroyer once for a non-null handle. `raw()` borrows a copy of the raw
handle; `grabRaw()` transfers ownership and leaves the wrapper null.

Batch construction is for independently stored raw handles. Add
`CORE_VK_BATCH_DESTROYABLE()` only when the backend supports vector-level
destruction; otherwise `VulkanRaiiVector` destroys non-null elements one by one.
Destruction context must not extend a parent lifetime: callers keep parent
wrappers alive until all children are destroyed.
