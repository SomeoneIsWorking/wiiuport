#include "wiiuport/title/ObjectIdentityScope.h"

#include "wiiuport/title/JsonBody.h"

namespace wiiuport::title {

void ObjectIdentityScope::bind(uint32_t object) {
    m_object.store(object, std::memory_order_release);
    m_binds.fetch_add(1, std::memory_order_relaxed);
    m_bindsInProgress.fetch_add(1, std::memory_order_relaxed);
}

uint32_t ObjectIdentityScope::current() const {
    m_queries.fetch_add(1, std::memory_order_relaxed);
    // The object published since the last read, taken before the slot is cleared so two
    // consecutive assemblies in the same binding do not disagree with each other about which
    // object they belong to.
    const uint32_t object = m_object.load(std::memory_order_acquire);
    if (object != 0) {
        m_queriesWithObject.fetch_add(1, std::memory_order_relaxed);
    }
    // The count for this interval, kept as the answer, and the interval's counter zeroed for
    // the next one.
    m_bindsAtLastQuery.store(m_bindsInProgress.exchange(0, std::memory_order_relaxed),
                             std::memory_order_relaxed);
    m_objectAtQuery.store(object, std::memory_order_relaxed);
    return object;
}

ObjectIdentityScope::Report ObjectIdentityScope::report() const {
    Report out;
    out.binds = m_binds.load(std::memory_order_relaxed);
    out.assemblyQueries = m_queries.load(std::memory_order_relaxed);
    out.assemblyQueriesWithObject = m_queriesWithObject.load(std::memory_order_relaxed);
    out.bindsSinceLastQuery = m_bindsAtLastQuery.load(std::memory_order_relaxed);
    out.lastObject = m_objectAtQuery.load(std::memory_order_relaxed);
    return out;
}

std::string ObjectIdentityScope::json() const {
    const Report r = report();
    JsonBody body;
    body.number("binds", r.binds);
    body.number("assemblyQueries", r.assemblyQueries);
    body.number("assemblyQueriesWithObject", r.assemblyQueriesWithObject);
    // The number of identities that might be wrong, and the number of reads to divide it by.
    // Zero is the answer that makes the correlation usable; anything else is stated rather than
    // discovered later.
    body.number("bindsSinceLastQuery", r.bindsSinceLastQuery);
    body.string("identitySource", "binderObject");
    const double share = r.assemblyQueries == 0 ? 0.0
                                                : static_cast<double>(r.assemblyQueriesWithObject) /
                                                      static_cast<double>(r.assemblyQueries);
    body.raw("queriesWithObjectShare", JsonBody::real(share, 6));
    return body.finish();
}

} // namespace wiiuport::title
