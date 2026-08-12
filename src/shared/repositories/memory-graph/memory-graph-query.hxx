#pragma once

#include <string>
#include <vector>

namespace memory_graph_query
{

inline constexpr const char* INSERT_ENTITY =
    "INSERT INTO memory_entity (kind, canonical, lang, created_at, "
    "updated_at, person_id) VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr const char* FIND_ENTITY_BY_NORM =
    "SELECT entity_id FROM memory_alias WHERE norm = ? AND lang = ? LIMIT 1";

inline constexpr const char* FIND_ENTITY_BY_FTS =
    "SELECT a.entity_id FROM memory_alias_fts "
    "JOIN memory_alias a ON a.id = memory_alias_fts.rowid "
    "WHERE memory_alias_fts MATCH ? ORDER BY a.confidence DESC LIMIT 1";

inline constexpr const char* FIND_ALIAS =
    "SELECT id FROM memory_alias WHERE entity_id = ? AND norm = ? LIMIT 1";

inline constexpr const char* INSERT_ALIAS =
    "INSERT INTO memory_alias (entity_id, surface, norm, lang, "
    "person_frame, confidence) VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr const char* FIND_ALIASES =
    "SELECT surface, person_frame FROM memory_alias "
    "WHERE entity_id = ? ORDER BY confidence DESC, id";

inline constexpr const char* FIND_OPEN_FACT =
    "SELECT id FROM memory_fact WHERE entity_id = ? AND predicate = ? "
    "AND scope = ? AND ref_id = ? AND valid_to = 0 LIMIT 1";

inline constexpr const char* INSERT_FACT =
    "INSERT INTO memory_fact (entity_id, predicate, value, canonical, "
    "type, priority, confidence, lang, scope, ref_id, valid_from, "
    "valid_to, hit_count, created_at, updated_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, ?, ?)";

inline constexpr const char* CLOSE_FACT =
    "UPDATE memory_fact SET valid_to = ?, updated_at = ? "
    "WHERE id = ? AND valid_to = 0";

inline constexpr const char* FIND_FACTS_BY_ENTITY = R"(
    WITH RECURSIVE reach(id, hops) AS (
      SELECT ?1, 0
      UNION ALL
      SELECT e.dst_id, r.hops + 1 FROM reach r
        JOIN memory_edge e ON e.kind = 'related' AND e.src_id = r.id
        WHERE r.hops < ?2
      UNION ALL
      SELECT e.src_id, r.hops + 1 FROM reach r
        JOIN memory_edge e ON e.kind = 'related' AND e.dst_id = r.id
        WHERE r.hops < ?2
    )
    SELECT f.id, f.entity_id, f.predicate, f.value, f.canonical, f.type,
           f.priority, f.confidence, f.hit_count, MIN(reach.hops) AS hops
    FROM reach JOIN memory_fact f ON f.entity_id = reach.id
    WHERE f.valid_to = 0 AND f.scope = ?3 AND f.ref_id = ?4
    GROUP BY f.id
    ORDER BY hops ASC, f.priority DESC, f.hit_count DESC
    LIMIT ?5)";

inline constexpr const char* FIND_FACTS_FTS =
    "SELECT f.id, f.entity_id, f.predicate, f.value, f.canonical, f.type, "
    "f.priority, f.confidence, f.hit_count "
    "FROM memory_fact_fts JOIN memory_fact f ON f.id = memory_fact_fts.rowid "
    "WHERE memory_fact_fts MATCH ? AND f.valid_to = 0 AND f.scope = ? "
    "AND f.ref_id = ? ORDER BY bm25(memory_fact_fts) LIMIT ?";

inline constexpr const char* FIND_VEC_NEIGHBOURS =
    "SELECT memory_id, distance FROM memory_vec "
    "WHERE embedding MATCH ? AND partition = ? "
    "ORDER BY distance LIMIT ?";

inline constexpr const char* FIND_FACT_BY_ID =
    "SELECT f.id, f.entity_id, f.predicate, f.value, f.canonical, f.type, "
    "f.priority, f.confidence, f.hit_count FROM memory_fact f "
    "WHERE f.id = ? AND f.valid_to = 0 AND f.scope = ? AND f.ref_id = ?";

inline constexpr const char* INSERT_EPISODE =
    "INSERT INTO memory_episode (kind, summary, actor, occurred_at, "
    "session_id, lang, scope, ref_id, salience, hit_count, "
    "last_recalled_at, decayed_at, rolled_up) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0, 0)";

inline constexpr const char* FIND_EPISODES_BETWEEN =
    "SELECT id FROM memory_episode "
    "WHERE scope = ? AND ref_id = ? AND occurred_at BETWEEN ? AND ? "
    "ORDER BY occurred_at DESC LIMIT ?";

inline constexpr const char* INSERT_SOURCE =
    "INSERT INTO memory_source (channel, turn_ref, at) VALUES (?, ?, ?)";

inline constexpr const char* INSERT_EDGE =
    "INSERT INTO memory_edge (kind, src_id, dst_id, predicate, since, "
    "until, ord) VALUES (?, ?, ?, ?, ?, ?, ?)";

inline constexpr const char* BUMP_FACT_HITS =
    "UPDATE memory_fact SET hit_count = hit_count + 1 WHERE id = ?";

inline constexpr const char* FIND_PROCEDURE =
    "SELECT id FROM memory_procedure WHERE name = ? LIMIT 1";

inline constexpr const char* BUMP_PROCEDURE =
    "UPDATE memory_procedure SET uses = uses + 1, updated_at = ? WHERE id = ?";

inline constexpr const char* INSERT_PROCEDURE =
    "INSERT INTO memory_procedure (name, goal, steps, uses, successes, "
    "updated_at) VALUES (?, ?, ?, 1, 1, ?)";

inline constexpr const char* FIND_PROCEDURE_STEPS =
    "SELECT steps FROM memory_procedure WHERE goal MATCH ? "
    "ORDER BY successes DESC, uses DESC LIMIT 1";

inline constexpr const char* FIND_FACT_CONTENT =
    "SELECT scope, ref_id, canonical FROM memory_fact "
    "WHERE id = ? AND valid_to = 0";

inline constexpr const char* FIND_OPEN_FACT_IDS =
    "SELECT id FROM memory_fact WHERE valid_to = 0";

inline constexpr const char* INSERT_VEC_ROW =
    "INSERT INTO memory_vec (embedding, partition, memory_id, view) "
    "VALUES (?, ?, ?, ?)";

inline constexpr const char* FIND_VEC_DUP =
    "SELECT memory_id, distance FROM memory_vec "
    "WHERE embedding MATCH ? AND partition = ? AND memory_id != ? "
    "ORDER BY distance LIMIT 1";

inline constexpr const char* DELETE_VEC_ROWS =
    "DELETE FROM memory_vec WHERE memory_id = ?";

inline constexpr const char* FIND_ALIAS_GAZETTEER =
    "SELECT a.norm, a.surface, a.person_frame, a.entity_id, e.kind "
    "FROM memory_alias a JOIN memory_entity e ON e.id = a.entity_id";

inline constexpr const char* FIND_PERSONS =
    "SELECT id, name, alias FROM person WHERE deleted_at IS NULL";

inline constexpr const char* FIND_CAMERAS =
    "SELECT id, name FROM camera WHERE deleted_at IS NULL";

inline constexpr const char* FIND_ZONES = "SELECT id, name FROM zone";

inline constexpr const char* FIND_STREAMS =
    "SELECT id, label FROM camera_stream";

inline constexpr const char* FIND_LEGACY_ENTITY =
    "SELECT id FROM memory_entity WHERE kind = 'concept' AND "
    "canonical = 'legacy' LIMIT 1";

inline constexpr const char* HAS_LEGACY_TABLE =
    "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND "
    "name = 'memory_l1'";

inline constexpr const char* INSERT_LEGACY_ENTITY =
    "INSERT INTO memory_entity (kind, canonical, lang, created_at, "
    "updated_at, person_id) VALUES ('concept', 'legacy', 'es', ?, ?, NULL)";

inline constexpr const char* FIND_LEGACY_ROWS =
    "SELECT id, scope, ref_id, type, content, priority, hit_count, "
    "created_at, updated_at FROM memory_l1 WHERE deleted_at IS NULL";

inline constexpr const char* INSERT_LEGACY_FACT =
    "INSERT INTO memory_fact (entity_id, predicate, value, canonical, "
    "type, priority, confidence, lang, scope, ref_id, valid_from, "
    "valid_to, hit_count, created_at, updated_at) "
    "VALUES (?, 'legacy', ?, ?, ?, ?, 0.5, 'es', ?, ?, ?, 0, ?, ?, ?)";

} // namespace memory_graph_query
