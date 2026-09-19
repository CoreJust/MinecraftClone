#include <client/PreviewResidency.hpp>

#include <algorithm>

namespace client {

namespace {

[[nodiscard]] bool present(PreviewHandle const& handle) noexcept
{
    return static_cast<bool>(handle);
}

[[nodiscard]] bool before(PreviewChunkKey const left, PreviewChunkKey const right) noexcept
{
    if (left.x != right.x) {
        return left.x < right.x;
    }
    if (left.y != right.y) {
        return left.y < right.y;
    }
    return left.z < right.z;
}

} // namespace

void PreviewResidency::advanceRevision(const PreviewRevision revision) noexcept
{
    if (revision == m_revision) {
        return;
    }
    m_revision = revision;
    ++m_change_serial;
    m_pending.clear();
    m_residents.clear();
    m_resident_bytes = 0;
    m_pending_upload_bytes = 0;
}

PreviewRequestResult PreviewResidency::request(
    const PreviewChunkKey key,
    const PreviewLevel level,
    const PreviewRevision revision
)
{
    if (revision != m_revision) {
        return { .admission = PreviewRequestAdmission::StaleRevision };
    }
    std::erase_if(m_pending, [key, level](Pending const& pending) {
        return pending.request.key() == key && pending.request.level() == level;
    });
    if (m_pending.size() >= m_limits.max_pending_requests) {
        return { .admission = PreviewRequestAdmission::Saturated };
    }
    PreviewRequest value{ m_next_token++, key, revision, level };
    m_pending.push_back(Pending{ .request = value });
    return { .admission = PreviewRequestAdmission::Accepted, .request = std::move(value) };
}

void PreviewResidency::cancel(PreviewRequest const& request) noexcept
{
    for (Pending& pending : m_pending) {
        if (pending.request.m_token == request.m_token) {
            pending.cancelled = true;
            return;
        }
    }
}

PreviewReplacementResult PreviewResidency::replace(
    PreviewRequest const& request,
    std::vector<uint8_t> bytes
)
{
    auto const pending = std::find_if(m_pending.begin(), m_pending.end(), [&request](Pending const& value) {
        return value.request.m_token == request.m_token;
    });
    if (pending == m_pending.end() || request.revision() != m_revision) {
        return { .replacement = PreviewReplacement::Stale };
    }
    if (pending->cancelled) {
        m_pending.erase(pending);
        return { .replacement = PreviewReplacement::Cancelled };
    }
    PreviewReplacementResult result = accept(
        request.key(), request.revision(), request.level(), request.m_token, std::move(bytes)
    );
    if (result.replacement != PreviewReplacement::Published) {
        m_pending.erase(pending);
    } else {
        m_pending.erase(pending);
    }
    return result;
}

PreviewReplacementResult PreviewResidency::accept(
    const PreviewChunkKey key,
    const PreviewRevision revision,
    const PreviewLevel level,
    const uint64_t token,
    std::vector<uint8_t> bytes
)
{
    if (revision != m_revision || token == 0U || bytes.empty()
        || bytes.size() > shared::PREVIEW_MAX_PAYLOAD_BYTES) {
        return { .replacement = PreviewReplacement::Stale };
    }
    Resident* chunk = resident(key);
    uint64_t* previous_token = chunk == nullptr
        ? nullptr
        : (level == PreviewLevel::Coarse ? &chunk->coarse_token : &chunk->final_token);
    if (previous_token != nullptr && token <= *previous_token) {
        return { .replacement = PreviewReplacement::Stale };
    }
    if (level == PreviewLevel::Coarse && chunk != nullptr && present(chunk->final)) {
        return { .replacement = PreviewReplacement::Superseded };
    }
    uint64_t const size = bytes.size();
    PreviewHandle old = chunk == nullptr ? nullptr : (level == PreviewLevel::Coarse
        ? chunk->coarse : chunk->final);
    uint64_t const old_size = old == nullptr ? 0U : old->byteSize();
    if (old != nullptr) {
        m_resident_bytes -= old_size;
        bool const pending = level == PreviewLevel::Coarse ? chunk->coarse_pending : chunk->final_pending;
        if (pending) {
            m_pending_upload_bytes -= old_size;
        }
    }
    if (size > m_limits.max_resident_bytes
        || size > m_limits.max_pending_upload_bytes
        || m_pending_upload_bytes > m_limits.max_pending_upload_bytes - size
        || !makeRoom(key, size)) {
        m_resident_bytes += old_size;
        if (old != nullptr) {
            bool const pending = level == PreviewLevel::Coarse ? chunk->coarse_pending : chunk->final_pending;
            if (pending) {
                m_pending_upload_bytes += old_size;
            }
        }
        return { .replacement = PreviewReplacement::BudgetExceeded };
    }
    if (chunk == nullptr) {
        m_residents.push_back(Resident{ .key = key });
        chunk = &m_residents.back();
    }
    PreviewHandle handle{ new PreviewMesh{ key, revision, level, std::move(bytes) } };
    if (level == PreviewLevel::Coarse) {
        chunk->coarse = handle;
        chunk->coarse_token = token;
        chunk->coarse_pending = true;
    } else {
        chunk->final = handle;
        chunk->final_token = token;
        chunk->final_pending = true;
    }
    ++m_change_serial;
    chunk->touch = m_touch++;
    m_resident_bytes += size;
    m_pending_upload_bytes += size;
    return { .replacement = PreviewReplacement::Published, .handle = std::move(handle) };
}

bool PreviewResidency::markUploaded(PreviewHandle const& handle) noexcept
{
    if (handle == nullptr || handle->revision() != m_revision) {
        return false;
    }
    Resident* const chunk = resident(handle->key());
    if (chunk == nullptr) {
        return false;
    }
    bool* pending = handle->level() == PreviewLevel::Coarse ? &chunk->coarse_pending : &chunk->final_pending;
    PreviewHandle const& stored = handle->level() == PreviewLevel::Coarse ? chunk->coarse : chunk->final;
    if (stored.get() != handle.get() || !*pending) {
        return false;
    }
    *pending = false;
    m_pending_upload_bytes -= handle->byteSize();
    m_uploaded_bytes += handle->byteSize();
    return true;
}

bool PreviewResidency::evict(const PreviewChunkKey key) noexcept
{
    Resident* const chunk = resident(key);
    if (chunk == nullptr || (!present(chunk->coarse) && !present(chunk->final))) {
        return false;
    }
    remove(*chunk);
    ++m_eviction_count;
    ++m_change_serial;
    return true;
}

PreviewSelection PreviewResidency::select(
    const PreviewChunkKey key,
    const PreviewLevel requested_level,
    const PreviewRevision revision
) const noexcept
{
    if (revision != m_revision) {
        return { .kind = PreviewSelectionKind::Fallback };
    }
    Resident const* const chunk = resident(key);
    if (chunk == nullptr) {
        return { .kind = PreviewSelectionKind::Fallback };
    }
    if (requested_level == PreviewLevel::Final && present(chunk->final)) {
        return { .kind = PreviewSelectionKind::Final, .handle = chunk->final };
    }
    if (present(chunk->coarse)) {
        return { .kind = PreviewSelectionKind::Coarse, .handle = chunk->coarse };
    }
    return { .kind = PreviewSelectionKind::Fallback };
}

PreviewResidencyStats PreviewResidency::stats() const noexcept
{
    PreviewResidencyStats result{
        .resident_bytes = m_resident_bytes,
        .pending_upload_bytes = m_pending_upload_bytes,
        .uploaded_bytes = m_uploaded_bytes,
        .eviction_count = m_eviction_count,
    };
    result.pending_requests = static_cast<uint32_t>(m_pending.size());
    for (Resident const& chunk : m_residents) {
        bool const coarse = present(chunk.coarse);
        bool const final = present(chunk.final);
        result.resident_chunks += static_cast<uint32_t>(coarse || final);
        result.resident_meshes += static_cast<uint32_t>(coarse) + static_cast<uint32_t>(final);
    }
    return result;
}

std::vector<PreviewHandle> PreviewResidency::handles() const
{
    std::vector<PreviewHandle> result;
    result.reserve(m_residents.size());
    for (Resident const& resident : m_residents) {
        PreviewHandle const& handle = resident.final ? resident.final : resident.coarse;
        if (handle) {
            result.push_back(handle);
        }
    }
    return result;
}

PreviewResidency::Resident* PreviewResidency::resident(const PreviewChunkKey key) noexcept
{
    auto const found = std::find_if(m_residents.begin(), m_residents.end(), [key](Resident const& item) {
        return item.key == key;
    });
    return found == m_residents.end() ? nullptr : &*found;
}

PreviewResidency::Resident const* PreviewResidency::resident(const PreviewChunkKey key) const noexcept
{
    auto const found = std::find_if(m_residents.cbegin(), m_residents.cend(), [key](Resident const& item) {
        return item.key == key;
    });
    return found == m_residents.cend() ? nullptr : &*found;
}

void PreviewResidency::remove(Resident& value) noexcept
{
    for (PreviewHandle const& handle : { value.coarse, value.final }) {
        if (handle != nullptr) {
            m_resident_bytes -= handle->byteSize();
        }
    }
    if (value.coarse_pending) {
        m_pending_upload_bytes -= value.coarse->byteSize();
    }
    if (value.final_pending) {
        m_pending_upload_bytes -= value.final->byteSize();
    }
    auto const found = std::find_if(m_residents.begin(), m_residents.end(), [&value](Resident const& item) {
        return &item == &value;
    });
    if (found != m_residents.end()) {
        m_residents.erase(found);
    }
}

bool PreviewResidency::makeRoom(const PreviewChunkKey protected_key, const uint64_t bytes) noexcept
{
    auto count = [this]() {
        uint32_t result = 0;
        for (Resident const& chunk : m_residents) {
            result += static_cast<uint32_t>(present(chunk.coarse) || present(chunk.final));
        }
        return result;
    };
    while (m_resident_bytes > m_limits.max_resident_bytes
        || bytes > m_limits.max_resident_bytes - std::min(m_resident_bytes, m_limits.max_resident_bytes)
        || (resident(protected_key) == nullptr && count() >= m_limits.max_resident_chunks)) {
        auto victim = m_residents.end();
        for (auto candidate = m_residents.begin(); candidate != m_residents.end(); ++candidate) {
            if (candidate->key == protected_key || (!present(candidate->coarse) && !present(candidate->final))) {
                continue;
            }
            if (victim == m_residents.end() || candidate->touch < victim->touch
                || (candidate->touch == victim->touch && before(candidate->key, victim->key))) {
                victim = candidate;
            }
        }
        if (victim == m_residents.end()) {
            return false;
        }
        remove(*victim);
        ++m_eviction_count;
    }
    return true;
}

} // namespace client
