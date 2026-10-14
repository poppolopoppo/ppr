module;
#include "pP/Macros.h"
module game.colony.checkpoint;
import std;
namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(CheckpointCodec, info, none);

    // ------------------------------------------------------------------
    // codec helpers
    // ------------------------------------------------------------------

    namespace {
        constexpr u64 kMagic = 0x314e454757525050ull; // PPRWGEN1, little endian.
        class Writer {
        public:
            Array<u8> m_bytes;

            void integer(const u64 value, const u32 width) {
                for (u32 index = 0u; index < width; ++index) {
                    m_bytes.push_back(static_cast<u8>(value >> (8u * index)));
                }
            }

            void u32value(const u32 value) {
                integer(value, 4u);
            }

            void u64value(const u64 value) {
                integer(value, 8u);
            }

            void real(const double value) {
                u64value(std::bit_cast<u64>(value));
            }

            void boolean(const bool value) {
                integer(value ? 1u : 0u, 1u);
            }

            void string(const std::string &value) {
                u32value(static_cast<u32>(value.size()));
                m_bytes.insert(m_bytes.end(), value.begin(), value.end());
            }

            void count(const std::size_t value) {
                u32value(static_cast<u32>(value));
            }
        };

        class Reader {
            std::span<const u8> m_bytes;
            std::size_t m_offset{};

        public:
            std::error_code m_error{};

            explicit Reader(const std::span<const u8> bytes) : m_bytes(bytes) {
            }

            std::size_t remaining() const noexcept {
                return m_bytes.size() - m_offset;
            }

            std::span<const u8> take(const std::size_t size) {
                if (m_error) {
                    return {};
                }
                if (size > remaining()) {
                    m_error = sim::make_error_code(sim::errc::truncated);
                    return {};
                }
                const auto bytes = m_bytes.subspan(m_offset, size);
                m_offset += size;
                return bytes;
            }

            u64 integer(const u32 width) {
                const auto bytes = take(width);
                u64 value{};
                for (std::size_t index = 0u; index < bytes.size(); ++index) {
                    value |= static_cast<u64>(bytes[index]) << (8u * index);
                }
                return value;
            }

            u32 u32value() {
                return static_cast<u32>(integer(4u));
            }

            u64 u64value() {
                return integer(8u);
            }

            double real() {
                return std::bit_cast<double>(u64value());
            }

            bool boolean() {
                const auto value = integer(1u);
                if (value > 1u) {
                    m_error = sim::make_error_code(sim::errc::invalid_payload);
                }
                return value == 1u;
            }

            u32 count(const u32 maximum, const u32 minimum_record_bytes) {
                const auto value = u32value();
                if (m_error) {
                    return 0u;
                }
                if (value > maximum) {
                    m_error = sim::make_error_code(sim::errc::invalid_payload);
                    return 0u;
                }
                if (value > remaining() / minimum_record_bytes) {
                    m_error = sim::make_error_code(sim::errc::truncated);
                    return 0u;
                }
                return value;
            }

            std::string string() {
                const auto size = count(1024u, 1u);
                const auto bytes = take(size);
                return {bytes.begin(), bytes.end()};
            }
        };

        std::error_code encodeParticles(
            Writer &writer, const sim::MatterParticleSnapshot &snapshot, const IContext *const context) {
            writer.u64value(snapshot.m_next_id);
            writer.count(snapshot.m_chunks.size());

            for (const auto &batch: snapshot.m_chunks) {
                PPR_RETURN_ERROR_ON_FAIL(CheckpointCodec, context ? context->error() : std::error_code{});
                writer.u32value(sim::chunkIndexOf(batch.m_position));
                writer.count(batch.records().size());
                for (const auto &particle: batch.records()) {
                    writer.u64value(particle.m_id);
                    writer.real(particle.m_x_m);
                    writer.real(particle.m_y_m);
                    writer.real(particle.m_volume_m3);
                    writer.real(particle.m_mass_kg);
                    writer.real(particle.m_temperature_k);
                    writer.real(particle.m_bulk_density_kg_m3);
                    writer.integer(particle.m_material, 2u);
                    writer.integer(static_cast<u8>(particle.m_phase), 1u);
                }
            }
            return {};
        }

        std::error_code decodeParticles(
            Reader &reader, sim::MatterParticleSnapshot &snapshot, const IContext *const context) {
            snapshot.m_next_id = reader.u64value();
            snapshot.m_chunks.resize(reader.count(sim::kChunkCount, 8u));
            u32 total = 0u;

            for (auto &batch: snapshot.m_chunks) {
                PPR_RETURN_ERROR_ON_FAIL(CheckpointCodec, context ? context->error() : std::error_code{});
                const auto index = reader.u32value();
                batch.m_position = {index % sim::kChunksPerEdge, index / sim::kChunksPerEdge};
                const u32 count = reader.count(sim::kMaxMatterParticles - total, 59u);
                total += count;
                Array<sim::MatterParticle> particles(count);
                for (auto &particle: particles) {
                    particle.m_id = reader.u64value();
                    particle.m_x_m = reader.real();
                    particle.m_y_m = reader.real();
                    particle.m_volume_m3 = reader.real();
                    particle.m_mass_kg = reader.real();
                    particle.m_temperature_k = reader.real();
                    particle.m_bulk_density_kg_m3 = reader.real();
                    particle.m_material = static_cast<u16>(reader.integer(2u));
                    particle.m_phase = static_cast<sim::EGenerationMatter>(reader.integer(1u));
                }
                batch.m_particles = std::make_shared<const Array<sim::MatterParticle>>(std::move(particles));
            }
            return {};
        }

        void encodeMetadata(Writer &writer, const WorldCheckpoint &checkpoint) {
            writer.count(checkpoint.m_sites.size());
            for (const auto &site: checkpoint.m_sites) {
                writer.real(site.m_x_m);
                writer.real(site.m_y_m);
                writer.u32value(site.m_biome);
            }

            writer.count(checkpoint.m_geometry.m_instances.size());
            for (const auto &instance: checkpoint.m_geometry.m_instances) {
                writer.u32value(instance.m_feature);
                writer.u32value(instance.m_rule);
                writer.u32value(instance.m_ordinal);
                writer.u32value(std::bit_cast<u32>(instance.m_origin_x));
                writer.u32value(std::bit_cast<u32>(instance.m_origin_y));
                writer.boolean(instance.m_reflected);
            }

            writer.count(checkpoint.m_geometry.m_segments.size());
            for (const auto &segment: checkpoint.m_geometry.m_segments) {
                writer.u32value(segment.m_rule);
                writer.real(segment.m_x0_m);
                writer.real(segment.m_y0_m);
                writer.real(segment.m_x1_m);
                writer.real(segment.m_y1_m);
                writer.real(segment.m_radius_m);
            }

            writer.count(checkpoint.m_geometry.m_blobs.size());
            for (const auto &blob: checkpoint.m_geometry.m_blobs) {
                writer.u32value(blob.m_biome);
                writer.u32value(blob.m_deposit);
                writer.u32value(blob.m_ordinal);
                writer.real(blob.m_x_m);
                writer.real(blob.m_y_m);
                writer.real(blob.m_radius_m);
                writer.real(blob.m_stretch);
                writer.real(blob.m_angle);
                writer.real(blob.m_offset_x);
                writer.real(blob.m_offset_y);
            }

            writer.count(checkpoint.m_geometry.m_omitted_instances.size());
            for (const auto omitted: checkpoint.m_geometry.m_omitted_instances) {
                writer.u32value(omitted);
            }

            writer.u32value(checkpoint.m_geometry.m_worst_placement_attempts);
            writer.real(checkpoint.m_start.m_spawn_x_m);
            writer.real(checkpoint.m_start.m_spawn_y_m);
            writer.real(checkpoint.m_start.m_floor_y_m);
            writer.count(checkpoint.m_start.m_resource_fine_cells.size());
            for (const auto cells: checkpoint.m_start.m_resource_fine_cells) {
                writer.u32value(cells);
            }

            writer.u32value(checkpoint.m_matter.m_span_count);
            writer.u32value(checkpoint.m_matter.m_searched_cells);
            writer.u32value(checkpoint.m_matter.m_largest_component);
            writer.count(checkpoint.m_matter.m_regions.size());

            for (const auto &region: checkpoint.m_matter.m_regions) {
                writer.u64value(region.m_identity);
                writer.integer(region.m_material, 2u);
                writer.real(region.m_temperature_k);
                writer.real(region.m_bulk_density_kg_m3);
                writer.real(region.m_volume_m3);
                writer.real(region.m_mass_kg);
                writer.boolean(region.m_fill_height_m.has_value());
                if (region.m_fill_height_m) {
                    writer.real(*region.m_fill_height_m);
                }
                writer.count(region.m_spans.size());
                for (const auto span: region.m_spans) {
                    writer.u32value(span.m_y);
                    writer.u32value(span.m_begin_x);
                    writer.u32value(span.m_end_x);
                }
            }

            for (const auto time: checkpoint.m_report.m_phase_times) {
                writer.u64value(std::bit_cast<u64>(static_cast<i64>(time.count())));
            }

            writer.count(checkpoint.m_report.m_material_counts.size());
            for (const auto &count: checkpoint.m_report.m_material_counts) {
                writer.integer(count.m_material, 2u);
                writer.u64value(count.m_cells);
            }

            writer.u64value(checkpoint.m_report.m_terrain_checksum);
            writer.u64value(checkpoint.m_report.m_output_checksum);
            writer.u64value(checkpoint.m_report.m_grid_payload_bytes);
            // Allocation observations describe the generating process/build,
            // not durable world state. They are intentionally absent from v3.
        }

        void decodeMetadata(Reader &reader, WorldCheckpoint &checkpoint) {
            checkpoint.m_sites.resize(reader.count(sim::kMaxGenerationSites, 20u));
            for (auto &site: checkpoint.m_sites) {
                site.m_x_m = reader.real();
                site.m_y_m = reader.real();
                site.m_biome = reader.u32value();
            }

            checkpoint.m_geometry.m_instances.resize(reader.count(1025u, 21u));
            for (auto &instance: checkpoint.m_geometry.m_instances) {
                instance.m_feature = reader.u32value();
                instance.m_rule = reader.u32value();
                instance.m_ordinal = reader.u32value();
                instance.m_origin_x = std::bit_cast<i32>(reader.u32value());
                instance.m_origin_y = std::bit_cast<i32>(reader.u32value());
                instance.m_reflected = reader.boolean();
            }

            checkpoint.m_geometry.m_segments.resize(reader.count(sim::kMaxGenerationSegments, 44u));
            for (auto &segment: checkpoint.m_geometry.m_segments) {
                segment.m_rule = reader.u32value();
                segment.m_x0_m = reader.real();
                segment.m_y0_m = reader.real();
                segment.m_x1_m = reader.real();
                segment.m_y1_m = reader.real();
                segment.m_radius_m = reader.real();
            }

            checkpoint.m_geometry.m_blobs.resize(reader.count(4096u, 68u));
            for (auto &blob: checkpoint.m_geometry.m_blobs) {
                blob.m_biome = reader.u32value();
                blob.m_deposit = reader.u32value();
                blob.m_ordinal = reader.u32value();
                blob.m_x_m = reader.real();
                blob.m_y_m = reader.real();
                blob.m_radius_m = reader.real();
                blob.m_stretch = reader.real();
                blob.m_angle = reader.real();
                blob.m_offset_x = reader.real();
                blob.m_offset_y = reader.real();
            }

            checkpoint.m_geometry.m_omitted_instances.resize(reader.count(1024u, 4u));
            for (auto &omitted: checkpoint.m_geometry.m_omitted_instances) {
                omitted = reader.u32value();
            }

            checkpoint.m_geometry.m_worst_placement_attempts = reader.u32value();
            checkpoint.m_start.m_spawn_x_m = reader.real();
            checkpoint.m_start.m_spawn_y_m = reader.real();
            checkpoint.m_start.m_floor_y_m = reader.real();
            checkpoint.m_start.m_resource_fine_cells.resize(reader.count(kMaxStartResources, 4u));
            for (auto &cells: checkpoint.m_start.m_resource_fine_cells) {
                cells = reader.u32value();
            }

            checkpoint.m_matter.m_span_count = reader.u32value();
            checkpoint.m_matter.m_searched_cells = reader.u32value();
            checkpoint.m_matter.m_largest_component = reader.u32value();
            checkpoint.m_matter.m_regions.resize(reader.count(sim::kMaxInitialMatterRegions, 47u));
            u32 spans = 0u;

            for (auto &region: checkpoint.m_matter.m_regions) {
                region.m_identity = reader.u64value();
                region.m_material = static_cast<u16>(reader.integer(2u));
                region.m_temperature_k = reader.real();
                region.m_bulk_density_kg_m3 = reader.real();
                region.m_volume_m3 = reader.real();
                region.m_mass_kg = reader.real();
                if (reader.boolean()) {
                    region.m_fill_height_m = reader.real();
                }
                region.m_spans.resize(reader.count(sim::kMaxInitialMatterSpans - spans, 12u));
                spans += static_cast<u32>(region.m_spans.size());
                for (auto &span: region.m_spans) {
                    span.m_y = reader.u32value();
                    span.m_begin_x = reader.u32value();
                    span.m_end_x = reader.u32value();
                }
            }

            for (auto &time: checkpoint.m_report.m_phase_times) {
                time = TimeSpan{std::bit_cast<i64>(reader.u64value())};
            }

            checkpoint.m_report.m_material_counts.resize(reader.count(sim::kMaxGenerationMaterials + 1u, 10u));
            for (auto &count: checkpoint.m_report.m_material_counts) {
                count.m_material = static_cast<u16>(reader.integer(2u));
                count.m_cells = reader.u64value();
            }

            checkpoint.m_report.m_terrain_checksum = reader.u64value();
            checkpoint.m_report.m_output_checksum = reader.u64value();
            checkpoint.m_report.m_grid_payload_bytes = reader.u64value();
        }
    }

    // ------------------------------------------------------------------
    // encode
    // ------------------------------------------------------------------

    Expected<Array<u8>> encodeWorldCheckpoint(
        const WorldCheckpoint &checkpoint, const ContentCatalog &catalog, const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, validateWorldCheckpoint(checkpoint, catalog, context));

        const auto snapshot = sim::save(checkpoint.m_snapshot);
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, snapshot);
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, context ? context->error() : std::error_code{});

        Writer writer;
        writer.u64value(kMagic);
        writer.u32value(kWorldCheckpointVersion);
        writer.boolean(true); // Full, never a delta.

        const auto &descriptor = checkpoint.m_descriptor;
        writer.u32value(descriptor.m_pack_schema_version);
        writer.u32value(descriptor.m_generator_version);
        writer.string(descriptor.m_profile);
        writer.u64value(descriptor.m_seed);
        writer.u64value(descriptor.m_content_fingerprint);
        writer.u32value(descriptor.m_metric.m_cells_per_metre);
        writer.u32value(descriptor.m_metric.m_cells_per_tile);
        writer.real(descriptor.m_metric.m_reference_thickness_m);

        writer.count(checkpoint.m_materials.size());
        for (const auto &material: checkpoint.m_materials) {
            writer.integer(material.m_id, 2u);
            writer.string(material.m_key);
        }

        writer.u64value(checkpoint.m_tick);
        writer.u32value(checkpoint.m_tick_hz);
        writer.boolean(checkpoint.m_initial_matter_valid);
        writer.boolean(checkpoint.m_controlled.has_value());
        if (checkpoint.m_controlled) {
            writer.u32value(checkpoint.m_controlled->m_index);
            writer.u32value(checkpoint.m_controlled->m_generation);
        }

        encodeMetadata(writer, checkpoint);

        u64 particle_bytes = 12u + checkpoint.m_particles.m_chunks.size() * 8u;
        for (const auto &batch: checkpoint.m_particles.m_chunks) {
            particle_bytes += batch.records().size() * 59u;
        }

        const u64 encoded_size = writer.m_bytes.size() + particle_bytes + 8u + snapshot->size();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec, encoded_size <= kMaxWorldCheckpointBytes
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::value_too_large));
        writer.m_bytes.reserve(static_cast<std::size_t>(encoded_size));

        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, encodeParticles(writer, checkpoint.m_particles, context));

        writer.u64value(snapshot->size());
        writer.m_bytes.insert(writer.m_bytes.end(), snapshot->begin(), snapshot->end());

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec, writer.m_bytes.size() <= kMaxWorldCheckpointBytes
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::value_too_large));
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, context ? context->error() : std::error_code{});
        return std::move(writer.m_bytes);
    }

    // ------------------------------------------------------------------
    // decode
    // ------------------------------------------------------------------

    Expected<WorldCheckpoint> decodeWorldCheckpoint(
        const std::span<const u8> bytes, const ContentCatalog &catalog, const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec, bytes.size() <= kMaxWorldCheckpointBytes
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::value_too_large));
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, context ? context->error() : std::error_code{});

        Reader reader{bytes};
        const auto magic = reader.u64value();
        const auto version = reader.u32value();
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, reader.m_error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec, magic == kMagic and version == kWorldCheckpointVersion
                                 ? std::error_code{}
                                 : std::make_error_code(std::errc::protocol_not_supported));
        const bool full = reader.boolean();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec, full ? std::error_code{} : sim::make_error_code(sim::errc::invalid_payload));

        WorldCheckpoint checkpoint;
        auto &descriptor = checkpoint.m_descriptor;
        descriptor.m_pack_schema_version = reader.u32value();
        descriptor.m_generator_version = reader.u32value();
        descriptor.m_profile = reader.string();
        descriptor.m_seed = reader.u64value();
        descriptor.m_content_fingerprint = reader.u64value();
        descriptor.m_metric.m_cells_per_metre = reader.u32value();
        descriptor.m_metric.m_cells_per_tile = reader.u32value();
        descriptor.m_metric.m_reference_thickness_m = reader.real();

        checkpoint.m_materials.resize(reader.count(kMaxContentMaterials + 1u, 6u));
        for (auto &material: checkpoint.m_materials) {
            material.m_id = static_cast<u16>(reader.integer(2u));
            material.m_key = reader.string();
        }

        checkpoint.m_tick = reader.u64value();
        checkpoint.m_tick_hz = reader.u32value();
        checkpoint.m_initial_matter_valid = reader.boolean();
        if (reader.boolean()) {
            const auto index = reader.u32value();
            const auto generation = reader.u32value();
            checkpoint.m_controlled = sim::Entity{index, generation};
        }

        decodeMetadata(reader, checkpoint);

        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, decodeParticles(reader, checkpoint.m_particles, context));

        const auto snapshot_size = reader.u64value();
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, reader.m_error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec, snapshot_size <= kMaxWorldCheckpointBytes and snapshot_size <= reader.remaining()
                                 ? std::error_code{}
                                 : sim::make_error_code(sim::errc::truncated));

        const auto snapshot_bytes = reader.take(static_cast<std::size_t>(snapshot_size));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            CheckpointCodec,
            reader.remaining() == 0u ? std::error_code{} : sim::make_error_code(sim::errc::invalid_payload));
        auto snapshot = sim::load(snapshot_bytes);
        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, snapshot);
        checkpoint.m_snapshot = std::move(*snapshot);

        PPR_RETURN_UNEXPECTED_ON_FAIL(CheckpointCodec, validateWorldCheckpoint(checkpoint, catalog, context));
        return checkpoint;
    }
}
