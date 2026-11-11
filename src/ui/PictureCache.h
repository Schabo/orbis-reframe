#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// The pictures an overlay renderer (titles, gauges) made lately, by what
// they show: most recently used first, kept by number and by size so a few
// big export pictures don't pile up. `Picture` holds its pixels in a shared
// `rgba` vector (null = none) and is empty when default-made; `Key` compares
// with ==. Thread-safe.
template <class Key, class Picture> class PictureCache {
public:
    PictureCache(size_t maxCount, size_t maxBytes) : m_maxCount(maxCount), m_maxBytes(maxBytes) {}

    // The picture kept for `key`, now the most recent; else an empty one.
    Picture find(const Key &key)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        for (auto it = m_list.begin(); it != m_list.end(); ++it)
            if (it->key == key) {
                Entry hit = *it;
                m_list.erase(it);
                m_list.push_front(hit);
                return hit.picture;
            }
        return {};
    }

    // Keeps `picture` for `key` and returns it. Pictures are drawn outside
    // the lock, so two threads may both draw one: the first kept wins, and
    // the other gets that one back.
    Picture insert(const Key &key, Picture picture)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        for (const Entry &e : m_list)
            if (e.key == key)
                return e.picture;
        m_list.push_front({key, std::move(picture)});
        m_bytes += m_list.front().bytes();
        while (m_list.size() > 1 && (m_list.size() > m_maxCount || m_bytes > m_maxBytes)) {
            m_bytes -= m_list.back().bytes();
            m_list.pop_back();
        }
        return m_list.front().picture;
    }

private:
    struct Entry {
        Key key;
        Picture picture;
        size_t bytes() const { return picture.rgba ? picture.rgba->size() : 0; }
    };

    const size_t m_maxCount;
    const size_t m_maxBytes;
    std::mutex m_mx;
    std::deque<Entry> m_list; // most recent first
    size_t m_bytes = 0;
};
