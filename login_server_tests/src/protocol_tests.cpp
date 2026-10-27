// Framing and codec tests (LOGIN-003): every frame shape round-trips; malformed frames are refused.
#include "common_types.h"
#include "fixed_string.h"
#include "frame_codec.h"
#include "frame_header.h"
#include "span_reader.h"
#include "span_writer.h"
#include "test_runner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

namespace tests
{
	namespace
	{
		using namespace hb::net;

		constexpr std::uint32_t test_max_body = 256;

		#pragma pack(push, 1)
		struct test_head
		{
			std::uint32_t token;
			std::uint8_t count;
		};

		struct test_record
		{
			std::uint16_t id;
			std::uint8_t flag;
		};
		#pragma pack(pop)

		static_assert(sizeof(test_head) == 5 && sizeof(test_record) == 3);

		std::vector<std::byte> filled(std::size_t size, std::uint8_t seed)
		{
			std::vector<std::byte> bytes(size);
			for (std::size_t i = 0; i < size; ++i)
				bytes[i] = static_cast<std::byte>(seed + i);
			return bytes;
		}

		// A frame whose header fields are chosen by the test, valid or not.
		std::vector<std::byte> raw_frame(const frame_header& header, std::span<const std::byte> body)
		{
			span_writer writer;
			writer.write(header);
			writer.write_array(body);
			return writer.take();
		}

		character_summary sample_summary()
		{
			character_summary summary{};
			summary.name.assign("Shadow");
			summary.is_valid = 1;
			summary.sex = 2;
			summary.skin = 3;
			summary.level = 180;
			summary.experience = 0x01020304;
			summary.stats = { 200, 10, 20, 30, 40, 50 };
			summary.appearance.hair_style = 7;
			summary.appearance.helm_colour = 15;
			summary.map_name.assign("aresden");
			summary.last_saved = { 2026, 10, 26, 23, 59 };
			return summary;
		}

		bool same(const character_summary& a, const character_summary& b)
		{
			const auto bytes_a = std::as_bytes(std::span<const character_summary, 1>(&a, 1));
			const auto bytes_b = std::as_bytes(std::span<const character_summary, 1>(&b, 1));
			return std::ranges::equal(bytes_a, bytes_b);
		}

		void fixed_string_tests(runner& t)
		{
			name_text name;
			t.check(name.view().empty(), "fixed_string starts empty");
			t.check(name.assign("Shadow") && name.view() == "Shadow", "fixed_string assign and view");
			t.check(name.is_zero_padded(), "assigned fixed_string is zero padded");
			t.check(name.assign("0123456789abcdef") && name.view().size() == 16, "fixed_string holds all N chars");
			t.check(name.is_zero_padded(), "full fixed_string counts as padded");
			t.check(!name.assign("0123456789abcdefg") && name.view() == "0123456789abcdef",
				"fixed_string refuses N+1 chars and keeps its value");
			t.check(!name.assign(std::string_view("ab\0cd", 5)), "fixed_string refuses an embedded NUL");
			t.check(name.assign("ab") && name.chars[2] == '\0' && name.chars[15] == '\0', "shorter assign clears the tail");
			name_text hidden;
			hidden.chars[0] = 'a';
			hidden.chars[5] = 'x';
			t.check(hidden.view() == "a" && !hidden.is_zero_padded(), "bytes behind the NUL are detected");
		}

		void reader_writer_tests(runner& t)
		{
			span_writer writer;
			const std::uint32_t word = 0x11223344;
			writer.write(word);
			const std::vector<std::byte> bytes = writer.take();
			t.check(bytes.size() == 4 && bytes[0] == std::byte{ 0x44 } && bytes[3] == std::byte{ 0x11 },
				"writer is little-endian");
			t.check(writer.size() == 0, "take empties the writer");

			span_reader reader(bytes);
			std::uint16_t half = 0;
			std::uint32_t whole = 0;
			t.check(reader.read(half) && half == 0x3344 && reader.remaining() == 2, "reader takes sizeof(T)");
			t.check(!reader.read(whole) && reader.remaining() == 2, "short read fails and consumes nothing");
			t.check(reader.read(half) && half == 0x1122 && reader.at_end(), "reader reaches the end");

			std::vector<test_record> records;
			span_reader empty({});
			t.check(empty.read_array(0, records) && records.empty(), "zero-count array reads");
			span_reader tiny(bytes);
			t.check(!tiny.read_array(2, records) && records.empty() && tiny.remaining() == 4,
				"array longer than input fails cleanly");
			span_reader huge(bytes);
			t.check(!huge.read_array(static_cast<std::size_t>(-1), records), "overflowing count fails");

			const character_summary summary = sample_summary();
			span_writer summary_writer;
			summary_writer.write(summary);
			character_summary read_back{};
			t.check(summary_writer.size() == 73 && read_exact(summary_writer.bytes(), read_back) && same(summary, read_back),
				"character_summary round-trips whole");
			t.check(read_back.name.view() == "Shadow" && read_back.map_name.view() == "aresden", "summary text survives");

			std::vector<std::byte> long_body(summary_writer.bytes().begin(), summary_writer.bytes().end());
			long_body.push_back(std::byte{ 0 });
			t.check(!read_exact(long_body, read_back), "read_exact refuses a long body");
			long_body.resize(72);
			t.check(!read_exact(long_body, read_back), "read_exact refuses a short body");
		}

		void counted_tests(runner& t)
		{
			const auto count_of = [](const test_head& head) { return head.count; };
			const std::array<test_record, 3> sent{ { { 1, 0 }, { 2, 1 }, { 0xBEEF, 1 } } };
			span_writer writer;
			writer.write(test_head{ 0xCAFE, 3 });
			writer.write_array(std::span<const test_record>(sent));
			test_head head{};
			std::vector<test_record> got;
			t.check(read_counted(writer.bytes(), head, got, count_of, 8) && head.token == 0xCAFE && got.size() == 3
				&& got[2].id == 0xBEEF && got[1].flag == 1, "counted body round-trips");

			span_writer none;
			none.write(test_head{ 1, 0 });
			t.check(read_counted(none.bytes(), head, got, count_of, 8) && got.empty(), "counted body with no records");

			t.check(!read_counted(writer.bytes(), head, got, count_of, 2), "count above max_count refused");
			std::vector<std::byte> body(writer.bytes().begin(), writer.bytes().end());
			body.push_back(std::byte{ 0 });
			t.check(!read_counted(body, head, got, count_of, 8) && got.empty(), "counted body one byte long refused");
			body.resize(body.size() - 1 - sizeof(test_record));
			t.check(!read_counted(body, head, got, count_of, 8), "counted body one record short refused");
			body.resize(sizeof(test_head) + 1);
			t.check(!read_counted(body, head, got, count_of, 8), "partial record refused");
			body.resize(3);
			t.check(!read_counted(body, head, got, count_of, 8), "truncated head refused");
		}

		void frame_tests(runner& t)
		{
			// Every shape: empty, one byte, a whole struct, a counted body, exactly the cap.
			std::vector<std::vector<std::byte>> bodies;
			bodies.push_back({});
			bodies.push_back(filled(1, 9));
			{
				span_writer writer;
				writer.write(sample_summary());
				bodies.push_back(writer.take());
			}
			{
				span_writer writer;
				const std::array<test_record, 2> records{ { { 5, 1 }, { 6, 0 } } };
				writer.write(test_head{ 7, 2 });
				writer.write_array(std::span<const test_record>(records));
				bodies.push_back(writer.take());
			}
			bodies.push_back(filled(test_max_body, 1));

			std::vector<std::byte> stream;
			bool every_shape = true;
			for (std::size_t i = 0; i < bodies.size(); ++i)
			{
				std::vector<std::byte> one;
				const auto id = static_cast<std::uint16_t>(0x0100 + i);
				every_shape = every_shape && encode_frame(id, bodies[i], test_max_body, one);
				frame decoded;
				every_shape = every_shape && decode_frame(one, test_max_body, decoded) == frame_error::none
					&& decoded.header.message_id == id && decoded.header.protocol_version == protocol_version
					&& decoded.header.reserved == 0 && decoded.header.body_size == bodies[i].size()
					&& decoded.body == bodies[i];
				stream.insert(stream.end(), one.begin(), one.end());
			}
			t.check(every_shape, "every frame shape round-trips through encode_frame / decode_frame");

			std::vector<std::byte> message_frame;
			frame decoded;
			character_summary summary_back{};
			t.check(encode_message(0x0201, sample_summary(), test_max_body, message_frame)
				&& message_frame.size() == sizeof(frame_header) + sizeof(character_summary)
				&& decode_frame(message_frame, test_max_body, decoded) == frame_error::none
				&& read_exact(decoded.body, summary_back) && same(summary_back, sample_summary()),
				"encode_message round-trips a struct");

			// The stream decoder: all frames in one append, then fed one byte at a time.
			frame_decoder bulk(test_max_body);
			const bool bulk_appended = bulk.append(stream);
			std::size_t bulk_count = 0;
			bool bulk_match = bulk_appended;
			while (bulk.next(decoded) == decode_status::frame_ready)
			{
				bulk_match = bulk_match && bulk_count < bodies.size() && decoded.body == bodies[bulk_count];
				++bulk_count;
			}
			t.check(bulk_match && bulk_count == bodies.size() && bulk.buffered() == 0, "decoder splits one append into frames");

			frame_decoder trickle(test_max_body);
			std::size_t trickle_count = 0;
			bool trickle_match = true;
			bool early = false;
			for (const std::byte byte : stream)
			{
				early = early || !trickle.append(std::span<const std::byte>(&byte, 1));
				decode_status status;
				while ((status = trickle.next(decoded)) == decode_status::frame_ready)
				{
					trickle_match = trickle_match && trickle_count < bodies.size() && decoded.body == bodies[trickle_count];
					++trickle_count;
				}
				early = early || status == decode_status::failed;
			}
			t.check(!early && trickle_match && trickle_count == bodies.size() && trickle.buffered() == 0,
				"decoder reassembles frames fed byte by byte");

			frame_decoder partial(test_max_body);
			t.check(partial.append(std::span<const std::byte>(stream).first(5))
				&& partial.next(decoded) == decode_status::need_more && partial.buffered() == 5, "partial header waits");
		}

		void rejection_tests(runner& t)
		{
			const std::vector<std::byte> body = filled(4, 0);
			frame decoded;
			std::vector<std::byte> out;

			t.check(!encode_frame(0x0100, filled(test_max_body + 1, 0), test_max_body, out) && out.empty(),
				"encoder refuses an oversize body");

			const auto refused = [&](const frame_header& header, std::span<const std::byte> frame_body, frame_error want)
			{
				const std::vector<std::byte> bytes = raw_frame(header, frame_body);
				if (decode_frame(bytes, test_max_body, decoded) != want)
					return false;
				// The stream decoder refuses in append() from the header alone and stays failed.
				frame_decoder decoder(test_max_body);
				if (decoder.append(std::span<const std::byte>(bytes).first(sizeof(frame_header))) || decoder.error() != want)
					return false;
				if (decoder.next(decoded) != decode_status::failed)
					return false;
				return !decoder.append(raw_frame({ 0, 0x0100, protocol_version, 0 }, {}))
					&& decoder.next(decoded) == decode_status::failed && decoder.buffered() == 0;
			};
			t.check(refused({ test_max_body + 1, 0x0100, protocol_version, 0 }, {}, frame_error::oversize),
				"oversize frame refused before its body arrives");
			t.check(refused({ 0xFFFFFFFF, 0x0100, protocol_version, 0 }, {}, frame_error::oversize),
				"maximum u32 body_size refused");
			t.check(refused({ 4, 0x0100, 2, 0 }, body, frame_error::bad_version), "bad version refused");
			t.check(refused({ 4, 0x0100, 0, 0 }, body, frame_error::bad_version), "version 0 refused");
			t.check(refused({ 4, 0x0100, protocol_version, 1 }, body, frame_error::reserved_nonzero),
				"nonzero reserved refused");

			const frame_header four{ 4, 0x0100, protocol_version, 0 };
			t.check(decode_frame(raw_frame(four, filled(3, 0)), test_max_body, decoded) == frame_error::short_frame,
				"short body refused");
			t.check(decode_frame(raw_frame(four, filled(5, 0)), test_max_body, decoded) == frame_error::long_frame,
				"long body refused");
			t.check(decode_frame(std::span<const std::byte>(raw_frame(four, body)).first(7), test_max_body, decoded)
				== frame_error::short_frame, "truncated header refused");
			t.check(decode_frame({}, test_max_body, decoded) == frame_error::short_frame, "empty input refused");
			t.check(to_string(frame_error::oversize) == "oversize" && to_string(frame_error::long_frame) == "long_frame",
				"frame errors have names for the log");
		}

		// append() accepts any number of complete frames and bounds only the incomplete tail.
		void overrun_tests(runner& t)
		{
			frame decoded;
			const std::vector<std::byte> empty_frame = raw_frame({ 0, 0x0100, protocol_version, 0 }, {});
			const std::vector<std::byte> small_frame = raw_frame({ 4, 0x0101, protocol_version, 0 }, filled(4, 7));
			constexpr std::size_t one_frame = sizeof(frame_header) + test_max_body;

			// Ordinary TCP batching: one 4 KiB-plus read of many small valid frames, far past one frame.
			std::vector<std::byte> batch;
			for (int i = 0; i < 400; ++i)
				batch.insert(batch.end(), small_frame.begin(), small_frame.end());
			frame_decoder batched(test_max_body);
			const bool batch_accepted = batched.append(batch);
			std::size_t batch_count = 0;
			bool batch_match = true;
			while (batched.next(decoded) == decode_status::frame_ready)
			{
				batch_match = batch_match && decoded.header.message_id == 0x0101 && decoded.body == filled(4, 7);
				++batch_count;
			}
			t.check(batch.size() > 2 * one_frame && batch_accepted && batch_match && batch_count == 400
				&& batched.buffered() == 0 && batched.error() == frame_error::none,
				std::format("{} small frames in one {}-byte append all decode", batch_count, batch.size()));

			// The same batch plus a partial frame: the tail waits, then completes.
			frame_decoder tailed(test_max_body);
			std::vector<std::byte> with_tail = batch;
			with_tail.insert(with_tail.end(), small_frame.begin(), small_frame.begin() + 5);
			std::size_t tailed_count = 0;
			const bool tailed_accepted = tailed.append(with_tail);
			while (tailed.next(decoded) == decode_status::frame_ready)
				++tailed_count;
			const bool tail_waits = tailed.buffered() == 5;
			const bool tail_done = tailed.append(std::span<const std::byte>(small_frame).subspan(5))
				&& tailed.next(decoded) == decode_status::frame_ready && tailed.buffered() == 0;
			t.check(tailed_accepted && tailed_count == 400 && tail_waits && tail_done, "batch with a partial tail decodes");

			// A tail longer than one frame: its header claims more than max_body_bytes; refused, buffer freed.
			std::vector<std::byte> long_tail = batch;
			const std::vector<std::byte> oversize_head = raw_frame({ test_max_body + 1, 0x0100, protocol_version, 0 }, {});
			long_tail.insert(long_tail.end(), oversize_head.begin(), oversize_head.end());
			const std::vector<std::byte> filler = filled(one_frame + 16, 0);
			long_tail.insert(long_tail.end(), filler.begin(), filler.end());
			frame_decoder too_long(test_max_body);
			t.check(!too_long.append(long_tail) && too_long.error() == frame_error::oversize && too_long.buffered() == 0
				&& too_long.next(decoded) == decode_status::failed, "a tail longer than one frame is refused in append");

			// A bad header after good frames in the same read is found by the walk, not left for next().
			std::vector<std::byte> bad_mid = batch;
			const std::vector<std::byte> bad_version_frame = raw_frame({ 0, 0x0100, 9, 0 }, {});
			bad_mid.insert(bad_mid.end(), bad_version_frame.begin(), bad_version_frame.end());
			frame_decoder walker(test_max_body);
			t.check(!walker.append(bad_mid) && walker.error() == frame_error::bad_version, "bad header behind good frames refused");

			// Undrained appends across many reads keep working (memory is the caller's to bound by draining).
			frame_decoder undrained(test_max_body);
			bool undrained_ok = true;
			for (int i = 0; i < 1000 && undrained_ok; ++i)
				undrained_ok = undrained.append(empty_frame);
			std::size_t undrained_count = 0;
			while (undrained.next(decoded) == decode_status::frame_ready)
				++undrained_count;
			t.check(undrained_ok && undrained_count == 1000 && undrained.buffered() == 0, "undrained appends decode later");

			const std::vector<std::byte> full = raw_frame({ test_max_body, 0x0100, protocol_version, 0 }, filled(test_max_body, 3));
			frame_decoder exact(test_max_body);
			t.check(exact.append(full) && exact.next(decoded) == decode_status::frame_ready && decoded.body.size() == test_max_body,
				"one append of header + max_body_bytes is accepted");

			frame_decoder partial(test_max_body);
			const std::span<const std::byte> almost = std::span<const std::byte>(full).first(full.size() - 1);
			t.check(partial.append(almost) && partial.next(decoded) == decode_status::need_more, "partial maximum frame waits");
			t.check(!partial.append(filled(2 * one_frame, 0)) && partial.error() == frame_error::bad_version
				&& partial.buffered() == 0, "garbage after a completed frame is refused at its header");

			frame_decoder drained(test_max_body);
			bool steady = true;
			for (int i = 0; i < 1000 && steady; ++i)
				steady = drained.append(empty_frame) && drained.next(decoded) == decode_status::frame_ready;
			t.check(steady && drained.buffered() == 0, "a drained decoder accepts frames indefinitely");
			t.check(to_string(frame_error::overrun) == "overrun", "overrun has a name for the log");
		}

		// max_body_bytes is capped so size sums cannot wrap a 32-bit size_t (review item 2).
		void ceiling_tests(runner& t)
		{
			static_assert(capped_body_bytes(0xFFFFFFFFu) == max_body_ceiling);
			static_assert(capped_body_bytes(65536) == 65536);
			static_assert(2 * (sizeof(frame_header) + std::uint64_t{ max_body_ceiling }) < 0xFFFFFFFFu);
			const frame_decoder decoder(0xFFFFFFFFu);
			t.check(decoder.max_body_bytes() == max_body_ceiling, "decoder caps max_body_bytes at the ceiling");
			t.check(check_header({ max_body_ceiling + 1, 0x0100, protocol_version, 0 }, 0xFFFFFFFFu) == frame_error::oversize,
				"header above the ceiling refused whatever the configured cap");
			t.check(check_header({ max_body_ceiling, 0x0100, protocol_version, 0 }, 0xFFFFFFFFu) == frame_error::none,
				"header at the ceiling accepted");
			std::vector<std::byte> out;
			t.check(!encode_frame(0x0100, filled(std::size_t{ max_body_ceiling } + 1, 0), 0xFFFFFFFFu, out) && out.empty(),
				"encoder refuses a body above the ceiling");
		}
	}

	void protocol_tests(runner& t)
	{
		fixed_string_tests(t);
		reader_writer_tests(t);
		counted_tests(t);
		frame_tests(t);
		rejection_tests(t);
		overrun_tests(t);
		ceiling_tests(t);
	}
}
