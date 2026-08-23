/* Backend-neutral subset of Kitty's terminal graphics protocol.
 *
 * The first implementation deliberately accepts only inline (t=d) data.  This
 * is the transport which remains meaningful across SSH, and it avoids exposing
 * the terminal emulator's local filesystem to programs on a remote host.
 */
#include "graphics.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_FAILURE_USERMSG
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

#define GR_ENCODED_MAX GRAPHICS_DATA_MAX
#define GR_IMAGE_MAX        (64U * 1024U * 1024U)
#define GR_TOTAL_MAX       (320U * 1024U * 1024U)
#define GR_IMAGE_COUNT_MAX         1024U
#define GR_PLACEMENT_COUNT_MAX     1024U
#define GR_DIMENSION_MAX           8192U
#define GR_PLACEMENT_CELLS_MAX     4096U

#define FIELD_ACTION    (1ULL << 0)
#define FIELD_MORE      (1ULL << 1)
#define FIELD_QUIET     (1ULL << 2)
#define FIELD_ID        (1ULL << 3)
#define FIELD_NUMBER    (1ULL << 4)
#define FIELD_FORMAT    (1ULL << 5)
#define FIELD_MEDIUM    (1ULL << 6)
#define FIELD_WIDTH     (1ULL << 7)
#define FIELD_HEIGHT    (1ULL << 8)
#define FIELD_COMPRESS  (1ULL << 9)
#define FIELD_PLACE     (1ULL << 10)
#define FIELD_DELETE    (1ULL << 11)
#define FIELD_COLUMNS   (1ULL << 12)
#define FIELD_ROWS      (1ULL << 13)
#define FIELD_SRC_X     (1ULL << 14)
#define FIELD_SRC_Y     (1ULL << 15)
#define FIELD_SRC_W     (1ULL << 16)
#define FIELD_SRC_H     (1ULL << 17)
#define FIELD_PIX_X     (1ULL << 18)
#define FIELD_PIX_Y     (1ULL << 19)
#define FIELD_Z         (1ULL << 20)
#define FIELD_CURSOR    (1ULL << 21)
#define FIELD_UNICODE   (1ULL << 22)
#define FIELD_SIZE      (1ULL << 23)
#define FIELD_OFFSET    (1ULL << 24)
#define FIELD_TRANSIENT (1ULL << 25)
#define FIELD_SELECTED  (1ULL << 26)

#define CONTINUATION_FIELDS (FIELD_MORE | FIELD_QUIET)

typedef struct {
	char action;
	char medium;
	char compression;
	char delete_action;
	int format;
	int more;
	int quiet;
	int has_id;
	int has_number;
	uint32_t id;
	uint32_t number;
	uint32_t placement_id;
	uint32_t width;
	uint32_t height;
	uint32_t source_x;
	uint32_t source_y;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t columns;
	uint32_t rows;
	uint32_t pixel_x;
	uint32_t pixel_y;
	int32_t z;
	int no_cursor_move;
	int unicode_placeholder;
	int selected;
	uint64_t fields;
} GraphicsCommand;

typedef struct GraphicsImage GraphicsImage;
typedef struct GraphicsPlacement GraphicsPlacement;

struct GraphicsImage {
	GraphicsImage *next;
	uint64_t serial;
	uint64_t access;
	uint32_t id;
	uint32_t number;
	int has_number;
	int width;
	int height;
	size_t bytes;
	size_t pixel_bytes;
	unsigned char *rgba;
	size_t encoded_bytes;
	unsigned char *encoded;
	unsigned int placements;
	int delete_candidate;
};

struct GraphicsPlacement {
	GraphicsPlacement *next;
	uint64_t serial;
	uint64_t access;
	GraphicsImage *image;
	uint32_t id;
	Line anchor;
	int column;
	int alt;
	int source_x;
	int source_y;
	int source_width;
	int source_height;
	int columns;
	int rows;
	int pixel_x;
	int pixel_y;
	int natural_size;
	int z;
	int selected;
};

typedef struct {
	int active;
	GraphicsCommand command;
	unsigned char *data;
	size_t length;
	size_t capacity;
} GraphicsTransfer;

static GraphicsImage *images;
static GraphicsPlacement *placements;
static GraphicsTransfer transfer;
static GraphicsImageFreeCallback image_free_callback;
static void *image_free_context;
static uint64_t next_serial = 1;
static uint64_t access_clock = 1;
static uint32_t next_image_id = 1;
static size_t total_image_bytes;
static size_t image_count;
static size_t placement_count;

static int
mul_overflow_size(size_t a, size_t b, size_t *result)
{
	if (a && b > SIZE_MAX / a)
		return 1;
	*result = a * b;
	return 0;
}

static int
parse_uint(const char *s, size_t len, uint32_t *value)
{
	uint64_t n = 0;
	size_t i;

	if (!len)
		return 0;
	for (i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		n = n * 10 + (unsigned int)(s[i] - '0');
		if (n > UINT32_MAX)
			return 0;
	}
	*value = (uint32_t)n;
	return 1;
}

static int
parse_int32(const char *s, size_t len, int32_t *value)
{
	uint32_t magnitude;
	int negative = 0;

	if (len && *s == '-') {
		negative = 1;
		s++;
		len--;
	}
	if (!parse_uint(s, len, &magnitude))
		return 0;
	if ((!negative && magnitude > INT32_MAX) ||
	    (negative && magnitude > (uint32_t)INT32_MAX + 1U))
		return 0;
	*value = negative ? (magnitude == (uint32_t)INT32_MAX + 1U ?
	    INT32_MIN : -(int32_t)magnitude) : (int32_t)magnitude;
	return 1;
}

static int
command_value_uint(const char *value, size_t length, uint32_t *destination)
{
	return parse_uint(value, length, destination);
}

static int
parse_command(const char *data, size_t length, GraphicsCommand *command,
		const char **payload, size_t *payload_length, const char **error)
{
	const char *header, *end, *separator, *field, *equals;
	size_t header_length;
	uint32_t number;

	memset(command, 0, sizeof(*command));
	command->action = 't';
	command->medium = 'd';
	command->format = 32;
	command->delete_action = 'a';
	if (!length || data[0] != 'G') {
		*error = "EINVAL:not a graphics command";
		return 0;
	}
	header = data + 1;
	end = data + length;
	separator = memchr(header, ';', (size_t)(end - header));
	if (separator) {
		header_length = (size_t)(separator - header);
		*payload = separator + 1;
		*payload_length = (size_t)(end - separator - 1);
	} else {
		header_length = (size_t)(end - header);
		*payload = end;
		*payload_length = 0;
	}
	if (header_length > GRAPHICS_HEADER_MAX) {
		*error = "E2BIG:control data too large";
		return 0;
	}
	if (*payload_length > GRAPHICS_PAYLOAD_MAX) {
		*error = "E2BIG:payload too large";
		return 0;
	}

	field = header;
	while (field < header + header_length) {
		const char *field_end = memchr(field, ',',
		    (size_t)(header + header_length - field));
		size_t field_length;
		const char *value;
		size_t value_length;
		char key;

		if (!field_end)
			field_end = header + header_length;
		field_length = (size_t)(field_end - field);
		equals = memchr(field, '=', field_length);
		if (!equals || equals != field + 1 || equals + 1 == field_end) {
			*error = "EINVAL:invalid control field";
			return 0;
		}
		key = field[0];
		value = equals + 1;
		value_length = (size_t)(field_end - value);

		switch (key) {
		case 'a':
			if (value_length != 1) goto invalid_value;
			command->action = *value;
			command->fields |= FIELD_ACTION;
			break;
		case 't':
			if (value_length != 1) goto invalid_value;
			command->medium = *value;
			command->fields |= FIELD_MEDIUM;
			break;
		case 'o':
			if (value_length != 1) goto invalid_value;
			command->compression = *value;
			command->fields |= FIELD_COMPRESS;
			break;
		case 'd':
			if (value_length != 1) goto invalid_value;
			command->delete_action = *value;
			command->fields |= FIELD_DELETE;
			break;
		case 'f':
			if (!command_value_uint(value, value_length, &number)) goto invalid_value;
			command->format = (int)number;
			command->fields |= FIELD_FORMAT;
			break;
		case 'm':
			if (!command_value_uint(value, value_length, &number) || number > 1) goto invalid_value;
			command->more = (int)number;
			command->fields |= FIELD_MORE;
			break;
		case 'q':
			if (!command_value_uint(value, value_length, &number) || number > 2) goto invalid_value;
			command->quiet = (int)number;
			command->fields |= FIELD_QUIET;
			break;
		case 'i':
			if (!command_value_uint(value, value_length, &command->id)) goto invalid_value;
			command->has_id = command->id != 0;
			command->fields |= FIELD_ID;
			break;
		case 'I':
			if (!command_value_uint(value, value_length, &command->number) || !command->number) goto invalid_value;
			command->has_number = 1;
			command->fields |= FIELD_NUMBER;
			break;
		case 'p':
			if (!command_value_uint(value, value_length, &command->placement_id)) goto invalid_value;
			command->fields |= FIELD_PLACE;
			break;
		case 's':
			if (!command_value_uint(value, value_length, &command->width)) goto invalid_value;
			command->fields |= FIELD_WIDTH;
			break;
		case 'v':
			if (!command_value_uint(value, value_length, &command->height)) goto invalid_value;
			command->fields |= FIELD_HEIGHT;
			break;
		case 'x':
			if (!command_value_uint(value, value_length, &command->source_x)) goto invalid_value;
			command->fields |= FIELD_SRC_X;
			break;
		case 'y':
			if (!command_value_uint(value, value_length, &command->source_y)) goto invalid_value;
			command->fields |= FIELD_SRC_Y;
			break;
		case 'w':
			if (!command_value_uint(value, value_length, &command->source_width)) goto invalid_value;
			command->fields |= FIELD_SRC_W;
			break;
		case 'h':
			if (!command_value_uint(value, value_length, &command->source_height)) goto invalid_value;
			command->fields |= FIELD_SRC_H;
			break;
		case 'c':
			if (!command_value_uint(value, value_length, &command->columns)) goto invalid_value;
			command->fields |= FIELD_COLUMNS;
			break;
		case 'r':
			if (!command_value_uint(value, value_length, &command->rows)) goto invalid_value;
			command->fields |= FIELD_ROWS;
			break;
		case 'X':
			if (!command_value_uint(value, value_length, &command->pixel_x)) goto invalid_value;
			command->fields |= FIELD_PIX_X;
			break;
		case 'Y':
			if (!command_value_uint(value, value_length, &command->pixel_y)) goto invalid_value;
			command->fields |= FIELD_PIX_Y;
			break;
		case 'z':
			if (!parse_int32(value, value_length, &command->z)) goto invalid_value;
			command->fields |= FIELD_Z;
			break;
		case 'C':
			if (!command_value_uint(value, value_length, &number) || number > 1) goto invalid_value;
			command->no_cursor_move = (int)number;
			command->fields |= FIELD_CURSOR;
			break;
		case 'U':
			if (!command_value_uint(value, value_length, &number) || number > 1) goto invalid_value;
			command->unicode_placeholder = (int)number;
			command->fields |= FIELD_UNICODE;
			break;
		case 'S':
			if (!command_value_uint(value, value_length, &number)) goto invalid_value;
			command->fields |= FIELD_SIZE;
			break;
		case 'O':
			if (!command_value_uint(value, value_length, &number)) goto invalid_value;
			command->fields |= FIELD_OFFSET;
			break;
		case 'N':
			if (!command_value_uint(value, value_length, &number)) goto invalid_value;
			command->fields |= FIELD_TRANSIENT;
			break;
		case 'V':
			/* st extension: applications with semantic selections can request
			 * the same atomic full-placement tint as a terminal selection. */
			if (!command_value_uint(value, value_length, &number) || number > 1) goto invalid_value;
			command->selected = (int)number;
			command->fields |= FIELD_SELECTED;
			break;
		default:
			/* Unknown keys are ignored for forward compatibility. */
			break;
		}
		field = field_end + (field_end < header + header_length);
	}
	if (command->has_id && command->has_number) {
		*error = "EINVAL:i and I are mutually exclusive";
		return 0;
	}
	return 1;

invalid_value:
	*error = "EINVAL:invalid control value";
	return 0;
}

static int
base64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static int
decode_base64(const char *source, size_t length, unsigned char **output,
		size_t *output_length)
{
	unsigned char *decoded;
	size_t full_length, i, remainder, used = 0;

	*output = NULL;
	*output_length = 0;
	remainder = length % 4;
	if (!length || remainder == 1 || length > GRAPHICS_PAYLOAD_MAX)
		return 0;
	decoded = malloc(length / 4 * 3 + (remainder ? remainder - 1 : 0) + 1);
	if (!decoded)
		return 0;
	full_length = length - remainder;
	for (i = 0; i < full_length; i += 4) {
		int a = base64_value((unsigned char)source[i]);
		int b = base64_value((unsigned char)source[i + 1]);
		int c = source[i + 2] == '=' ? -2 : base64_value((unsigned char)source[i + 2]);
		int d = source[i + 3] == '=' ? -2 : base64_value((unsigned char)source[i + 3]);
		if (a < 0 || b < 0 || c == -1 || d == -1 ||
		    (c == -2 && d != -2) ||
		    ((c == -2 || d == -2) && i + 4 != length)) {
			free(decoded);
			return 0;
		}
		decoded[used++] = (unsigned char)((a << 2) | (b >> 4));
		if (c != -2) {
			decoded[used++] = (unsigned char)((b << 4) | (c >> 2));
			if (d != -2)
				decoded[used++] = (unsigned char)((c << 6) | d);
		}
	}
	/* Kitty's protocol examples use padded base64, but kitten icat omits the
	 * final '=' or '==' when the source length is not divisible by three. */
	if (remainder) {
		int a = base64_value((unsigned char)source[i]);
		int b = base64_value((unsigned char)source[i + 1]);
		int c = remainder == 3 ?
		    base64_value((unsigned char)source[i + 2]) : 0;
		if (a < 0 || b < 0 || c < 0) {
			free(decoded);
			return 0;
		}
		decoded[used++] = (unsigned char)((a << 2) | (b >> 4));
		if (remainder == 3)
			decoded[used++] = (unsigned char)((b << 4) | (c >> 2));
	}
	if (used > GRAPHICS_DATA_MAX) {
		free(decoded);
		return 0;
	}
	*output = decoded;
	*output_length = used;
	return 1;
}

static int
append_transfer_data(const char *payload, size_t payload_length)
{
	unsigned char *decoded;
	size_t decoded_length, wanted, capacity;

	if (!decode_base64(payload, payload_length, &decoded, &decoded_length))
		return 0;
	if (decoded_length > GR_ENCODED_MAX - transfer.length) {
		free(decoded);
		return 0;
	}
	wanted = transfer.length + decoded_length;
	if (wanted > transfer.capacity) {
		unsigned char *grown;
		capacity = transfer.capacity ? transfer.capacity : 4096;
		while (capacity < wanted) {
			if (capacity > GR_ENCODED_MAX / 2) {
				capacity = GR_ENCODED_MAX;
				break;
			}
			capacity *= 2;
		}
		grown = realloc(transfer.data, capacity);
		if (!grown) {
			free(decoded);
			return 0;
		}
		transfer.data = grown;
		transfer.capacity = capacity;
	}
	memcpy(transfer.data + transfer.length, decoded, decoded_length);
	transfer.length += decoded_length;
	free(decoded);
	return 1;
}

static void
clear_transfer(void)
{
	free(transfer.data);
	memset(&transfer, 0, sizeof(transfer));
}

static GraphicsImage *
find_image_id(uint32_t id)
{
	GraphicsImage *image;
	for (image = images; image; image = image->next)
		if (image->id == id && id)
			return image;
	return NULL;
}

static GraphicsImage *
find_image_number(uint32_t number)
{
	GraphicsImage *image, *newest = NULL;
	for (image = images; image; image = image->next)
		if (image->has_number && image->number == number &&
		    (!newest || image->serial > newest->serial))
			newest = image;
	return newest;
}

static uint32_t
allocate_image_id(void)
{
	uint32_t start = next_image_id;
	do {
		uint32_t candidate = next_image_id++;
		if (!next_image_id)
			next_image_id = 1;
		if (candidate && !find_image_id(candidate))
			return candidate;
	} while (next_image_id != start);
	return 0;
}

static void
unlink_placement(GraphicsPlacement *placement)
{
	GraphicsPlacement **link;

	for (link = &placements; *link; link = &(*link)->next) {
		if (*link == placement) {
			*link = placement->next;
			if (placement->image->placements)
				placement->image->placements--;
			free(placement);
			placement_count--;
			return;
		}
	}
}

static void
delete_image(GraphicsImage *image)
{
	GraphicsPlacement *placement, *next;
	GraphicsImage **link;

	for (placement = placements; placement; placement = next) {
		next = placement->next;
		if (placement->image == image)
			unlink_placement(placement);
	}
	for (link = &images; *link; link = &(*link)->next) {
		if (*link == image) {
			*link = image->next;
			if (image_free_callback)
				image_free_callback(image->serial, image_free_context);
			total_image_bytes -= image->bytes;
			free(image->rgba);
			free(image->encoded);
			free(image);
			image_count--;
			return;
		}
	}
}

static void
collect_anonymous_images(void)
{
	GraphicsImage *image, *next;
	for (image = images; image; image = next) {
		next = image->next;
		if (!image->id && !image->placements)
			delete_image(image);
	}
}

static int
evict_unplaced(GraphicsImage *exclude)
{
	GraphicsImage *image, *oldest = NULL;
	for (image = images; image; image = image->next)
		if (image != exclude && !image->placements &&
		    (!oldest || image->access < oldest->access))
			oldest = image;
	if (!oldest)
		return 0;
	delete_image(oldest);
	return 1;
}

static int
make_room(size_t bytes, GraphicsImage *exclude)
{
	size_t used = total_image_bytes - (exclude ? exclude->bytes : 0);
	while (bytes > GR_TOTAL_MAX - used) {
		if (!evict_unplaced(exclude))
			return 0;
		used = total_image_bytes - (exclude ? exclude->bytes : 0);
	}
	return 1;
}

static int
inflate_data(const unsigned char *source, size_t source_length,
		unsigned char **output, size_t *output_length, size_t expected)
{
	z_stream stream;
	unsigned char *buffer;
	size_t capacity = expected ? expected : 65536;
	int status;

	if (!capacity || capacity > GR_IMAGE_MAX)
		return 0;
	buffer = malloc(capacity);
	if (!buffer)
		return 0;
	memset(&stream, 0, sizeof(stream));
	stream.next_in = (Bytef *)source;
	stream.avail_in = (uInt)source_length;
	if ((size_t)stream.avail_in != source_length || inflateInit(&stream) != Z_OK) {
		free(buffer);
		return 0;
	}
	for (;;) {
		stream.next_out = buffer + stream.total_out;
		stream.avail_out = (uInt)(capacity - stream.total_out);
		status = inflate(&stream, Z_NO_FLUSH);
		if (status == Z_STREAM_END)
			break;
		if (status != Z_OK || stream.avail_in == 0) {
			inflateEnd(&stream);
			free(buffer);
			return 0;
		}
		if (!stream.avail_out) {
			size_t new_capacity;
			if (capacity == GR_IMAGE_MAX) {
				inflateEnd(&stream);
				free(buffer);
				return 0;
			}
			new_capacity = capacity > GR_IMAGE_MAX / 2 ? GR_IMAGE_MAX : capacity * 2;
			buffer = realloc(buffer, new_capacity);
			if (!buffer) {
				inflateEnd(&stream);
				return 0;
			}
			capacity = new_capacity;
		}
	}
	*output_length = stream.total_out;
	inflateEnd(&stream);
	if (expected && *output_length != expected) {
		free(buffer);
		return 0;
	}
	*output = buffer;
	return 1;
}

static int
decode_image(const GraphicsCommand *command, const unsigned char *encoded,
		size_t encoded_length, unsigned char **rgba, int *width, int *height,
		const char **error)
{
	unsigned char *data = (unsigned char *)encoded, *inflated = NULL, *pixels;
	size_t data_length = encoded_length, expected = 0, count, i;
	int channels;

	*rgba = NULL;
	if (command->format == 24 || command->format == 32) {
		size_t normalized;
		if (!command->width || !command->height ||
		    command->width > GR_DIMENSION_MAX || command->height > GR_DIMENSION_MAX ||
		    mul_overflow_size(command->width, command->height, &count) ||
		    mul_overflow_size(count, command->format == 24 ? 3 : 4, &expected) ||
		    expected > GR_IMAGE_MAX ||
		    mul_overflow_size(count, 4, &normalized) ||
		    normalized > GR_IMAGE_MAX) {
			*error = "EINVAL:invalid raw image dimensions";
			return 0;
		}
	}
	if (command->compression) {
		if (command->compression != 'z' ||
		    !inflate_data(encoded, encoded_length, &inflated, &data_length, expected)) {
			*error = "EINVAL:invalid compressed image";
			return 0;
		}
		data = inflated;
	}
	if (command->format == 24 || command->format == 32) {
		if ((!command->compression && data_length != expected) ||
		    data_length != expected) {
			free(inflated);
			*error = "EINVAL:raw image size mismatch";
			return 0;
		}
		pixels = malloc((size_t)command->width * command->height * 4);
		if (!pixels) {
			free(inflated);
			*error = "ENOMEM:image allocation failed";
			return 0;
		}
		if (command->format == 32) {
			memcpy(pixels, data, (size_t)command->width * command->height * 4);
		} else {
			for (i = 0, count = (size_t)command->width * command->height; i < count; i++) {
				pixels[4*i] = data[3*i];
				pixels[4*i+1] = data[3*i+1];
				pixels[4*i+2] = data[3*i+2];
				pixels[4*i+3] = 255;
			}
		}
		*width = (int)command->width;
		*height = (int)command->height;
		*rgba = pixels;
		free(inflated);
		return 1;
	}
	if (command->format != 100 || data_length > INT_MAX ||
	    !stbi_info_from_memory(data, (int)data_length, width, height, &channels) ||
	    *width <= 0 || *height <= 0 || *width > (int)GR_DIMENSION_MAX ||
	    *height > (int)GR_DIMENSION_MAX ||
	    mul_overflow_size((size_t)*width, (size_t)*height, &count) ||
	    mul_overflow_size(count, 4, &expected) || expected > GR_IMAGE_MAX) {
		free(inflated);
		*error = "EINVAL:invalid PNG image";
		return 0;
	}
	pixels = stbi_load_from_memory(data, (int)data_length, width, height,
	    &channels, 4);
	free(inflated);
	if (!pixels) {
		*error = "EINVAL:PNG decode failed";
		return 0;
	}
	*rgba = pixels;
	return 1;
}

static void
set_response(GraphicsCommandResult *result, const GraphicsCommand *command,
		uint32_t id, uint32_t placement_id, int success, const char *message,
		int include_number)
{
	int length;

	/* q=1 suppresses successful acknowledgements; q=2 is fully quiet and
	 * suppresses failures as well (the mode used by host applications and
	 * Unicode-placeholder clients). */
	if ((success && command->quiet >= 1) ||
	    (!success && command->quiet >= 2))
		return;
	if (!id && command->action != 'q' && !command->has_number)
		return;
	if (include_number && command->has_number) {
		if (placement_id)
			length = snprintf(result->response, sizeof(result->response),
			    "\033_Gi=%u,I=%u,p=%u;%s\033\\", id, command->number,
			    placement_id, message);
		else
			length = snprintf(result->response, sizeof(result->response),
			    "\033_Gi=%u,I=%u;%s\033\\", id, command->number, message);
	} else if (placement_id) {
		length = snprintf(result->response, sizeof(result->response),
		    "\033_Gi=%u,p=%u;%s\033\\", id, placement_id, message);
	} else {
		length = snprintf(result->response, sizeof(result->response),
		    "\033_Gi=%u;%s\033\\", id, message);
	}
	if (length > 0 && (size_t)length < sizeof(result->response))
		result->response_len = (size_t)length;
}

static GraphicsImage *
store_image(const GraphicsCommand *command, unsigned char *rgba, int width,
		int height, const unsigned char *encoded, size_t encoded_bytes,
		const char **error)
{
	GraphicsImage *image, *old;
	unsigned char *encoded_copy = NULL;
	size_t pixel_bytes = (size_t)width * height * 4;
	size_t bytes;
	uint32_t id = command->id;

	/* Keep direct PNG bytes as a compact backing store.  Linux can then drop
	 * the much larger RGBA copy after uploading a visible texture and decode it
	 * again only if that scrollback image returns to the viewport. */
	if (command->format != 100 || command->compression == 'z')
		encoded_bytes = 0;
	if (encoded_bytes) {
		encoded_copy = malloc(encoded_bytes);
		if (!encoded_copy) {
			*error = "ENOMEM:image backing allocation failed";
			return NULL;
		}
		memcpy(encoded_copy, encoded, encoded_bytes);
	}
	if (encoded_bytes > SIZE_MAX - pixel_bytes) {
		free(encoded_copy);
		*error = "ENOMEM:image size overflow";
		return NULL;
	}
	bytes = pixel_bytes + encoded_bytes;

	if (command->has_number) {
		id = allocate_image_id();
		if (!id) {
			free(encoded_copy);
			*error = "ENOMEM:no image IDs available";
			return NULL;
		}
	}
	old = id ? find_image_id(id) : NULL;
	while (image_count >= GR_IMAGE_COUNT_MAX && !old)
		if (!evict_unplaced(NULL)) {
			free(encoded_copy);
			*error = "ENOMEM:too many images";
			return NULL;
		}
	if (!make_room(bytes, old)) {
		free(encoded_copy);
		*error = "ENOMEM:image quota exceeded";
		return NULL;
	}
	if (old)
		delete_image(old);
	image = calloc(1, sizeof(*image));
	if (!image) {
		free(encoded_copy);
		*error = "ENOMEM:image allocation failed";
		return NULL;
	}
	image->serial = next_serial++;
	image->access = access_clock++;
	image->id = id;
	image->number = command->number;
	image->has_number = command->has_number;
	image->width = width;
	image->height = height;
	image->bytes = bytes;
	image->pixel_bytes = pixel_bytes;
	image->rgba = rgba;
	image->encoded_bytes = encoded_bytes;
	image->encoded = encoded_copy;
	image->next = images;
	images = image;
	total_image_bytes += bytes;
	image_count++;
	return image;
}

static GraphicsPlacement *
find_placement(GraphicsImage *image, uint32_t id)
{
	GraphicsPlacement *placement;
	if (!id)
		return NULL;
	for (placement = placements; placement; placement = placement->next)
		if (placement->image == image && placement->id == id)
			return placement;
	return NULL;
}

static GraphicsPlacement *
place_image(GraphicsImage *image, const GraphicsCommand *command, Line anchor,
		int column, int alt, int cell_width, int cell_height,
		GraphicsCommandResult *result, const char **error)
{
	GraphicsPlacement *placement, *old;
	uint32_t sx = command->source_x, sy = command->source_y;
	uint32_t sw, sh, columns = command->columns, rows = command->rows;
	int natural;
	double aspect;

	if (!anchor || cell_width <= 0 || cell_height <= 0 ||
	    command->unicode_placeholder) {
		*error = command->unicode_placeholder ?
		    "ENOSYS:Unicode placeholders are not supported" :
		    "EINVAL:invalid placement anchor";
		return NULL;
	}
	if (sx >= (uint32_t)image->width || sy >= (uint32_t)image->height) {
		*error = "EINVAL:source rectangle is outside image";
		return NULL;
	}
	sw = command->source_width ? command->source_width : image->width - sx;
	sh = command->source_height ? command->source_height : image->height - sy;
	if (sw > (uint32_t)image->width - sx) sw = image->width - sx;
	if (sh > (uint32_t)image->height - sy) sh = image->height - sy;
	if (!sw || !sh || command->pixel_x >= (uint32_t)cell_width ||
	    command->pixel_y >= (uint32_t)cell_height) {
		*error = "EINVAL:invalid placement geometry";
		return NULL;
	}
	natural = !columns && !rows;
	aspect = (double)sw / sh;
	if (natural) {
		columns = (command->pixel_x + sw + cell_width - 1) / cell_width;
		rows = (command->pixel_y + sh + cell_height - 1) / cell_height;
	} else if (!rows) {
		double pixels = columns * (double)cell_width / aspect;
		rows = (uint32_t)(pixels / cell_height + 0.999999);
	} else if (!columns) {
		double pixels = rows * (double)cell_height * aspect;
		columns = (uint32_t)(pixels / cell_width + 0.999999);
	}
	if (!columns) columns = 1;
	if (!rows) rows = 1;
	if (columns > GR_PLACEMENT_CELLS_MAX || rows > GR_PLACEMENT_CELLS_MAX ||
	    placement_count >= GR_PLACEMENT_COUNT_MAX) {
		*error = "E2BIG:placement is too large";
		return NULL;
	}
	old = find_placement(image, command->placement_id);
	if (old)
		unlink_placement(old);
	placement = calloc(1, sizeof(*placement));
	if (!placement) {
		*error = "ENOMEM:placement allocation failed";
		return NULL;
	}
	placement->serial = next_serial++;
	placement->access = access_clock++;
	placement->image = image;
	placement->id = command->placement_id;
	placement->anchor = anchor;
	placement->column = column;
	placement->alt = !!alt;
	placement->source_x = (int)sx;
	placement->source_y = (int)sy;
	placement->source_width = (int)sw;
	placement->source_height = (int)sh;
	placement->columns = (int)columns;
	placement->rows = (int)rows;
	placement->pixel_x = (int)command->pixel_x;
	placement->pixel_y = (int)command->pixel_y;
	placement->natural_size = natural;
	placement->z = command->z;
	placement->selected = command->selected;
	placement->next = placements;
	placements = placement;
	placement_count++;
	image->placements++;
	image->access = access_clock++;
	result->redraw = 1;
	if (!command->no_cursor_move) {
		result->move_cursor = 1;
		result->columns = (int)columns;
		result->rows = (int)rows;
	}
	return placement;
}

static int
placement_intersects(const GraphicsPlacement *placement, int x, int y,
		int (*line_to_row)(Line, int))
{
	int row = line_to_row ? line_to_row(placement->anchor, placement->alt) :
	    INT_MIN;
	return row != INT_MIN && x >= placement->column &&
	    x < placement->column + placement->columns && y >= row &&
	    y < row + placement->rows;
}

static void
handle_delete(const GraphicsCommand *command, Line cursor_line,
		int cursor_column, int alt, int viewport_rows,
		int (*line_to_row)(Line, int), GraphicsCommandResult *result)
{
	GraphicsPlacement *placement, *next;
	GraphicsImage *image, *nextimage, *selected = NULL;
	int hard = command->delete_action >= 'A' && command->delete_action <= 'Z';
	int changed = 0;
	int cursor_row = line_to_row ? line_to_row(cursor_line, alt) : INT_MIN;
	char mode = hard ? (char)(command->delete_action - 'A' + 'a') :
	    command->delete_action;

	for (image = images; image; image = image->next)
		image->delete_candidate = 0;
	if (mode == 'i' && command->has_id)
		selected = find_image_id(command->id);
	else if (mode == 'n' && command->has_number)
		selected = find_image_number(command->number);
	if (hard && selected)
		selected->delete_candidate = 1;
	if (hard && mode == 'r')
		for (image = images; image; image = image->next)
			if (image->id >= command->source_x &&
			    image->id <= command->source_y)
				image->delete_candidate = 1;

	for (placement = placements; placement; placement = next) {
		int row, remove = 0;
		next = placement->next;
		row = line_to_row ? line_to_row(placement->anchor, placement->alt) :
		    INT_MIN;
		switch (mode) {
		case 'a':
			remove = placement->alt == !!alt && row != INT_MIN &&
			    row < viewport_rows && row + placement->rows > 0;
			break;
		case 'i': case 'n':
			remove = placement->image == selected &&
			    (!command->placement_id || placement->id == command->placement_id);
			break;
		case 'c':
			remove = cursor_row != INT_MIN && placement->alt == !!alt &&
			    placement_intersects(placement, cursor_column, cursor_row,
			    line_to_row);
			break;
		case 'p': case 'q':
			remove = command->source_x && command->source_y &&
			    placement->alt == !!alt &&
			    placement_intersects(placement, command->source_x - 1,
			    command->source_y - 1, line_to_row) &&
			    (mode != 'q' || placement->z == command->z);
			break;
		case 'x':
			remove = command->source_x && placement->alt == !!alt &&
			    command->source_x - 1 >= (uint32_t)placement->column &&
			    command->source_x - 1 <
			    (uint32_t)(placement->column + placement->columns);
			break;
		case 'y':
			remove = command->source_y && placement->alt == !!alt &&
			    row != INT_MIN && command->source_y - 1 >= (uint32_t)row &&
			    command->source_y - 1 < (uint32_t)(row + placement->rows);
			break;
		case 'z':
			remove = placement->z == command->z;
			break;
		case 'r':
			remove = placement->image->id >= command->source_x &&
			    placement->image->id <= command->source_y;
			break;
		default:
			break;
		}
		if (remove) {
			if (hard)
				placement->image->delete_candidate = 1;
			unlink_placement(placement);
			changed = 1;
		}
	}
	if (hard)
		for (image = images; image; image = nextimage) {
			nextimage = image->next;
			if (image->delete_candidate && !image->placements)
				delete_image(image);
		}
	collect_anonymous_images();
	if (changed)
		result->redraw = 1;
}

static int
process_data_command(const GraphicsCommand *command, const unsigned char *data,
		size_t length, Line anchor, int column, int alt, int cell_width,
		int cell_height, int available, GraphicsCommandResult *result)
{
	unsigned char *rgba;
	int width, height;
	GraphicsImage *image;
	GraphicsPlacement *placement = NULL;
	const char *error = NULL;
	uint32_t response_id = command->id;

	if (!available) {
		error = "ENOSYS:graphics renderer unavailable";
		goto failed;
	}
	if (command->medium != 'd') {
		error = "ENOSYS:only direct transmission is supported";
		goto failed;
	}
	if (!decode_image(command, data, length, &rgba, &width, &height, &error))
		goto failed;
	if (command->action == 'q') {
		stbi_image_free(rgba);
		set_response(result, command, response_id, 0, 1, "OK", 0);
		return 1;
	}
	image = store_image(command, rgba, width, height, data, length, &error);
	if (!image) {
		stbi_image_free(rgba);
		goto failed;
	}
	response_id = image->id;
	if (command->action == 'T') {
		placement = place_image(image, command, anchor, column, alt,
		    cell_width, cell_height, result, &error);
		if (!placement) {
			if (!image->id)
				delete_image(image);
			goto failed;
		}
	}
	if (command->action == 't' && !image->id) {
		delete_image(image);
		return 1;
	}
	set_response(result, command, response_id,
	    placement ? placement->id : 0, 1, "OK", command->has_number);
	return 1;

failed:
	set_response(result, command, response_id, command->placement_id, 0,
	    error ? error : "EINVAL:invalid image", 0);
	return 0;
}

static int
process_command(const GraphicsCommand *command, const unsigned char *data,
		size_t length, Line anchor, int column, int alt, int cell_width,
		int cell_height, int viewport_rows,
		int (*line_to_row)(Line, int), int available,
		GraphicsCommandResult *result)
{
	GraphicsImage *image;
	GraphicsPlacement *placement;
	const char *error = NULL;
	uint32_t response_id = command->id;

	switch (command->action) {
	case 't':
	case 'T':
	case 'q':
		return process_data_command(command, data, length, anchor, column,
		    alt, cell_width, cell_height, available, result);
	case 'p':
		if (!available) {
			error = "ENOSYS:graphics renderer unavailable";
			break;
		}
		image = command->has_id ? find_image_id(command->id) :
		    command->has_number ? find_image_number(command->number) : NULL;
		if (!image) {
			error = "ENOENT:image not found";
			break;
		}
		response_id = image->id;
		placement = place_image(image, command, anchor, column, alt,
		    cell_width, cell_height, result, &error);
		if (!placement)
			break;
		set_response(result, command, response_id, placement->id, 1, "OK",
		    command->has_number);
		return 1;
	case 'd':
		handle_delete(command, anchor, column, alt, viewport_rows,
		    line_to_row, result);
		return 1;
	default:
		error = "ENOSYS:graphics action is not supported";
		break;
	}
	set_response(result, command, response_id, command->placement_id, 0,
	    error, 0);
	return 0;
}

int
graphics_handle_apc(const char *data, size_t length, Line anchor,
		int column, int alt, int cell_width, int cell_height,
		int viewport_rows, int (*line_to_row)(Line, int), int available,
		GraphicsCommandResult *result)
{
	GraphicsCommand command;
	const char *payload, *error = NULL;
	size_t payload_length;
	unsigned char *decoded = NULL;
	size_t decoded_length = 0;
	int continuation;

	memset(result, 0, sizeof(*result));
	if (!length || data[0] != 'G')
		return 0;
	result->handled = 1;
	if (!parse_command(data, length, &command, &payload, &payload_length, &error)) {
		/* There may not be a trustworthy ID or quiet level after a parse error. */
		if (transfer.active)
			clear_transfer();
		return 1;
	}

	if (transfer.active && command.action == 'd' &&
	    (command.fields & FIELD_ACTION))
		clear_transfer();
	continuation = transfer.active &&
	    !(command.fields & ~CONTINUATION_FIELDS) &&
	    (command.fields & FIELD_MORE);
	if (transfer.active && continuation) {
		if (!append_transfer_data(payload, payload_length)) {
			GraphicsCommand saved = transfer.command;
			clear_transfer();
			set_response(result, &saved, saved.id, saved.placement_id, 0,
			    "EINVAL:invalid image chunk", 0);
			return 1;
		}
		if (command.fields & FIELD_QUIET)
			transfer.command.quiet = command.quiet;
		if (command.more)
			return 1;
		command = transfer.command;
		decoded = transfer.data;
		decoded_length = transfer.length;
		memset(&transfer, 0, sizeof(transfer));
		process_command(&command, decoded, decoded_length, anchor, column,
		    alt, cell_width, cell_height, viewport_rows, line_to_row,
		    available, result);
		free(decoded);
		return 1;
	}
	if (transfer.active)
		clear_transfer();

	if (command.action == 'd' && (command.fields & FIELD_ACTION)) {
		process_command(&command, NULL, 0, anchor, column, alt,
		    cell_width, cell_height, viewport_rows, line_to_row, available,
		    result);
		return 1;
	}
	if (command.action == 'p') {
		if (payload_length) {
			set_response(result, &command, command.id, command.placement_id, 0,
			    "EINVAL:placement has a payload", 0);
			return 1;
		}
		process_command(&command, NULL, 0, anchor, column, alt,
		    cell_width, cell_height, viewport_rows, line_to_row, available,
		    result);
		return 1;
	}
	if (!payload_length || !decode_base64(payload, payload_length,
	    &decoded, &decoded_length)) {
		set_response(result, &command, command.id, command.placement_id, 0,
		    "EINVAL:invalid base64 payload", 0);
		return 1;
	}
	if (command.more) {
		transfer.active = 1;
		transfer.command = command;
		transfer.data = decoded;
		transfer.length = decoded_length;
		transfer.capacity = decoded_length;
		return 1;
	}
	process_command(&command, decoded, decoded_length, anchor, column, alt,
	    cell_width, cell_height, viewport_rows, line_to_row, available,
	    result);
	free(decoded);
	return 1;
}

typedef struct {
	GraphicsPlacement *placement;
	int row;
} VisiblePlacement;

static int
placement_compare(const void *left, const void *right)
{
	const GraphicsPlacement *a = ((const VisiblePlacement *)left)->placement;
	const GraphicsPlacement *b = ((const VisiblePlacement *)right)->placement;
	if (a->z != b->z)
		return a->z < b->z ? -1 : 1;
	if (a->image->id != b->image->id)
		return a->image->id < b->image->id ? -1 : 1;
	return a->serial < b->serial ? -1 : a->serial > b->serial;
}

void
graphics_draw(int alt, int stage, int cell_width, int cell_height,
		int viewport_rows,
		int (*line_to_row)(Line, int), GraphicsDrawCallback draw,
		void *context)
{
	GraphicsPlacement *placement;
	VisiblePlacement *visible;
	GraphicsPlacementView view;
	size_t count = 0, i = 0;

	(void)cell_width;
	(void)cell_height;
	if (!draw || !line_to_row)
		return;
	for (placement = placements; placement; placement = placement->next) {
		int row;
		int placement_stage = placement->z < INT32_MIN / 2 ?
		    GRAPHICS_STAGE_BELOW_BACKGROUND : placement->z < 0 ?
		    GRAPHICS_STAGE_BELOW_TEXT : GRAPHICS_STAGE_ABOVE_TEXT;
		if (placement->alt != !!alt || placement_stage != stage)
			continue;
		row = line_to_row(placement->anchor, placement->alt);
		if (row != INT_MIN && row < viewport_rows &&
		    row + placement->rows > 0)
			count++;
	}
	if (!count)
		return;
	visible = malloc(count * sizeof(*visible));
	if (!visible)
		return;
	for (placement = placements; placement; placement = placement->next) {
		int row;
		int placement_stage = placement->z < INT32_MIN / 2 ?
		    GRAPHICS_STAGE_BELOW_BACKGROUND : placement->z < 0 ?
		    GRAPHICS_STAGE_BELOW_TEXT : GRAPHICS_STAGE_ABOVE_TEXT;
		if (placement->alt != !!alt || placement_stage != stage)
			continue;
		row = line_to_row(placement->anchor, placement->alt);
		if (row != INT_MIN && row < viewport_rows &&
		    row + placement->rows > 0)
			visible[i++] = (VisiblePlacement){placement, row};
	}
	qsort(visible, count, sizeof(*visible), placement_compare);
	for (i = 0; i < count; i++) {
		int row = visible[i].row;
		placement = visible[i].placement;
		memset(&view, 0, sizeof(view));
		view.serial = placement->image->serial;
		view.image_id = placement->image->id;
		view.placement_id = placement->id;
		view.rgba = placement->image->rgba;
		view.image_width = placement->image->width;
		view.image_height = placement->image->height;
		view.source_x = placement->source_x;
		view.source_y = placement->source_y;
		view.source_width = placement->source_width;
		view.source_height = placement->source_height;
		view.anchor = placement->anchor;
		view.column = placement->column;
		view.row = row;
		view.alt = placement->alt;
		view.columns = placement->columns;
		view.rows = placement->rows;
		view.pixel_x = placement->pixel_x;
		view.pixel_y = placement->pixel_y;
		view.natural_size = placement->natural_size;
		view.z = placement->z;
		view.selected = placement->selected;
		draw(&view, context);
		placement->access = placement->image->access = access_clock++;
	}
	free(visible);
}

void
graphics_recycle_line(Line line)
{
	GraphicsPlacement *placement, *next;
	if (!line)
		return;
	for (placement = placements; placement; placement = next) {
		next = placement->next;
		if (placement->anchor == line)
			unlink_placement(placement);
	}
	collect_anonymous_images();
}

void
graphics_reanchor_line(Line oldline, Line newline)
{
	GraphicsPlacement *placement;
	if (!oldline || oldline == newline)
		return;
	for (placement = placements; placement; placement = placement->next)
		if (placement->anchor == oldline)
			placement->anchor = newline;
}

void
graphics_reanchor_line_address(uintptr_t oldline, Line newline)
{
	GraphicsPlacement *placement;
	if (!oldline || !newline)
		return;
	for (placement = placements; placement; placement = placement->next)
		if ((uintptr_t)placement->anchor == oldline)
			placement->anchor = newline;
}

void
graphics_reflow_line(Line oldline, int logical_offset, int new_columns,
		long long first_output, GraphicsReflowLineAt line_at, void *context)
{
	GraphicsPlacement *placement, *next;

	if (!oldline || new_columns <= 0 || !line_at)
		return;
	for (placement = placements; placement; placement = next) {
		long long offset, output;
		Line newline;
		next = placement->next;
		if (placement->anchor != oldline)
			continue;
		offset = (long long)logical_offset + placement->column;
		output = first_output + offset / new_columns;
		newline = line_at(context, output);
		if (!newline) {
			unlink_placement(placement);
			continue;
		}
		placement->anchor = newline;
		placement->column = (int)(offset % new_columns);
	}
	collect_anonymous_images();
}

int
graphics_line_extent(Line line)
{
	GraphicsPlacement *placement;
	int extent = 0;

	for (placement = placements; placement; placement = placement->next)
		if (placement->anchor == line)
			extent = MAX(extent, placement->column + 1);
	return extent;
}

static void
png_write_u32(unsigned char *output, uint32_t value)
{
	output[0] = (unsigned char)(value >> 24);
	output[1] = (unsigned char)(value >> 16);
	output[2] = (unsigned char)(value >> 8);
	output[3] = (unsigned char)value;
}

static unsigned char *
png_write_chunk(unsigned char *output, const char type[4],
		const unsigned char *data, size_t length)
{
	uLong checksum;

	png_write_u32(output, (uint32_t)length);
	memcpy(output + 4, type, 4);
	if (length)
		memcpy(output + 8, data, length);
	checksum = crc32(0L, Z_NULL, 0);
	checksum = crc32(checksum, output + 4, 4);
	if (length)
		checksum = crc32(checksum, output + 8, (uInt)length);
	png_write_u32(output + 8 + length, (uint32_t)checksum);
	return output + 12 + length;
}

/* Encode the source rectangle represented by a placement as a standalone PNG.
 * Keeping this in the backend-neutral image store lets RGB/RGBA transmissions
 * and cropped placements be copied just like images originally sent as PNG. */
static int
placement_png(GraphicsPlacement *placement, unsigned char **png,
		size_t *png_length)
{
	GraphicsImage *image = placement->image;
	const unsigned char *pixels;
	unsigned char ihdr[13], *raw = NULL, *compressed = NULL, *output, *cursor;
	size_t stride, raw_length, output_length;
	uLongf compressed_length;
	int had_pixels, y;

	*png = NULL;
	*png_length = 0;
	if (placement->source_width <= 0 || placement->source_height <= 0 ||
	    (size_t)placement->source_width > (SIZE_MAX - 1) / 4)
		return 0;
	stride = (size_t)placement->source_width * 4 + 1;
	if ((size_t)placement->source_height > SIZE_MAX / stride)
		return 0;
	raw_length = stride * (size_t)placement->source_height;
	if (raw_length > ULONG_MAX)
		return 0;

	had_pixels = image->rgba != NULL;
	pixels = graphics_image_pixels(image->serial);
	if (!pixels)
		return 0;
	raw = malloc(raw_length);
	if (!raw)
		goto failed;
	for (y = 0; y < placement->source_height; y++) {
		unsigned char *row = raw + (size_t)y * stride;
		const unsigned char *source = pixels +
		    ((size_t)(placement->source_y + y) * image->width +
		    placement->source_x) * 4;
		row[0] = 0; /* PNG filter: None */
		memcpy(row + 1, source, stride - 1);
	}

	compressed_length = compressBound((uLong)raw_length);
	compressed = malloc((size_t)compressed_length);
	if (!compressed || compress2(compressed, &compressed_length, raw,
	    (uLong)raw_length, Z_DEFAULT_COMPRESSION) != Z_OK)
		goto failed;
	if ((size_t)compressed_length > UINT32_MAX ||
	    (size_t)compressed_length > SIZE_MAX - (8 + 25 + 12 + 12))
		goto failed;
	output_length = 8 + 25 + 12 + (size_t)compressed_length + 12;
	output = malloc(output_length);
	if (!output)
		goto failed;
	memcpy(output, "\x89PNG\r\n\x1a\n", 8);
	memset(ihdr, 0, sizeof(ihdr));
	png_write_u32(ihdr, (uint32_t)placement->source_width);
	png_write_u32(ihdr + 4, (uint32_t)placement->source_height);
	ihdr[8] = 8; /* bit depth */
	ihdr[9] = 6; /* RGBA */
	cursor = png_write_chunk(output + 8, "IHDR", ihdr, sizeof(ihdr));
	cursor = png_write_chunk(cursor, "IDAT", compressed,
	    (size_t)compressed_length);
	cursor = png_write_chunk(cursor, "IEND", NULL, 0);
	*png = output;
	*png_length = (size_t)(cursor - output);
	free(compressed);
	free(raw);
	if (!had_pixels && image->encoded)
		graphics_release_image_pixels(image->serial);
	return 1;

failed:
	free(compressed);
	free(raw);
	if (!had_pixels && image->encoded)
		graphics_release_image_pixels(image->serial);
	return 0;
}

/* Images are atomic selection objects: touching any occupied cell selects the
 * entire placement.  A system clipboard has only one image/png representation,
 * so publish one only when the selection identifies exactly one placement. */
int
graphics_selection_png(int alt, int (*line_to_row)(Line, int),
		GraphicsSelectionCallback selected, void *context,
		unsigned char **png, size_t *png_length)
{
	GraphicsPlacement *placement, *match = NULL;

	if (!png || !png_length)
		return 0;
	*png = NULL;
	*png_length = 0;
	if (!line_to_row || !selected)
		return 0;
	for (placement = placements; placement; placement = placement->next) {
		int row;
		if (placement->alt != !!alt)
			continue;
		row = line_to_row(placement->anchor, placement->alt);
		if (row == INT_MIN || !selected(placement->column, row,
		    placement->columns, placement->rows, context))
			continue;
		if (match)
			return 0;
		match = placement;
	}
	return match ? placement_png(match, png, png_length) : 0;
}

void
graphics_clear_buffer(int alt)
{
	GraphicsPlacement *placement, *next;
	for (placement = placements; placement; placement = next) {
		next = placement->next;
		if (placement->alt == !!alt)
			unlink_placement(placement);
	}
	collect_anonymous_images();
}

void
graphics_reset(void)
{
	GraphicsImage *image;
	clear_transfer();
	while ((image = images))
		delete_image(image);
	placements = NULL;
	total_image_bytes = image_count = placement_count = 0;
}

void
graphics_set_image_free_callback(GraphicsImageFreeCallback callback,
		void *context)
{
	image_free_callback = callback;
	image_free_context = context;
}

size_t graphics_image_bytes(void) { return total_image_bytes; }
size_t graphics_image_count(void) { return image_count; }
size_t graphics_placement_count(void) { return placement_count; }

const unsigned char *
graphics_image_pixels(uint64_t serial)
{
	GraphicsImage *image;
	unsigned char *rgba;
	int width, height, channels;

	for (image = images; image && image->serial != serial; image = image->next)
		;
	if (!image || image->rgba)
		return image ? image->rgba : NULL;
	if (!image->encoded || image->encoded_bytes > INT_MAX)
		return NULL;
	rgba = stbi_load_from_memory(image->encoded, (int)image->encoded_bytes,
	    &width, &height, &channels, 4);
	if (!rgba || width != image->width || height != image->height) {
		stbi_image_free(rgba);
		return NULL;
	}
	image->rgba = rgba;
	image->bytes += image->pixel_bytes;
	total_image_bytes += image->pixel_bytes;
	return image->rgba;
}

void
graphics_release_image_pixels(uint64_t serial)
{
	GraphicsImage *image;

	for (image = images; image && image->serial != serial; image = image->next)
		;
	if (!image || !image->rgba || !image->encoded)
		return;
	stbi_image_free(image->rgba);
	image->rgba = NULL;
	image->bytes -= image->pixel_bytes;
	total_image_bytes -= image->pixel_bytes;
}

void
graphics_compact_images(void)
{
	GraphicsImage *image;

	for (image = images; image; image = image->next)
		if (image->rgba && image->encoded)
			graphics_release_image_pixels(image->serial);
}

int
graphics_has_visible_placements(int alt, int viewport_rows,
		int (*line_to_row)(Line, int))
{
	GraphicsPlacement *placement;
	if (!line_to_row)
		return 0;
	for (placement = placements; placement; placement = placement->next) {
		int row;
		if (placement->alt != !!alt)
			continue;
		row = line_to_row(placement->anchor, placement->alt);
		if (row != INT_MIN && row < viewport_rows &&
		    row + placement->rows > 0)
			return 1;
	}
	return 0;
}
