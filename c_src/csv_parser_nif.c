#include <string.h>
#include <math.h>
#include <assert.h>
#include "erl_nif.h"
#include "libcsv/csv.h"

#define OPTION_DELIM_TABS 1
#define OPTION_RETURN_BINARY 2

typedef int bool;
#define true 1
#define false 0

#define MAX_PARSE_SIZE 128
// Empty lines are filtered out by libcsv (default behaviour). Each
// line consists therefor of at least 2 bytes (one character plus one
// new-line) except for the last line which doesn't need to end with
// new-line.
#define MAX_ROWS_PER_BATCH 64

#define min( a, b ) ( ((a) < (b)) ? (a) : (b) )

struct cell {
  char *data_ptr;
  size_t allocated_size;
  size_t data_size;
};

struct row_buffer {
  struct cell *cells_ptr;
  unsigned allocated_n;
  unsigned cols_used;
};

struct out_buffer {
  ERL_NIF_TERM rows[MAX_ROWS_PER_BATCH];
  unsigned row_n;
};

struct csv_buffer {
  char *data_ptr;
  size_t size;
  size_t consumed;
};

// The capture index is zero-based here while it's 1-indexed in the
// Erlang code.
struct capture {
  unsigned *indexes_ptr;
  unsigned size;
};

struct state {
  struct csv_parser parser;
  struct row_buffer row_buffer;
  struct csv_buffer csv_buffer;
  struct capture capture;
  unsigned options;
};

struct callback_state {
  struct out_buffer out_buffer;
  struct row_buffer *row_buffer_ptr;
  struct capture *capture_ptr;
  ErlNifEnv* env_ptr;
  unsigned options;
};

struct csv_chunk {
  char *data_ptr;
  size_t size;
};

ErlNifResourceType* state_type;

static size_t align_cell_size(size_t size)
{
  return ceil(size / 16.0) * 16;
}

static void ensure_cell_size(struct cell *cell_ptr, size_t size)
{
  size_t new_size;

  if (cell_ptr->data_ptr == NULL || cell_ptr->allocated_size < size) {
    if (cell_ptr->data_ptr != NULL) {
      enif_free(cell_ptr->data_ptr);
    }
    new_size = align_cell_size(size);
    cell_ptr->data_ptr = enif_alloc(new_size);
    cell_ptr->allocated_size = new_size;
  }
}

static struct cell empty_cell()
{
  struct cell cell;

  cell.data_ptr = NULL;
  cell.allocated_size = 0;
  cell.data_size = 0;
  return cell;
}

static bool is_output_column(struct callback_state* cb_state_ptr, int col_i)
{
  unsigned i;

  if (cb_state_ptr->capture_ptr->indexes_ptr == NULL) {
    return true;
  } else {
    for (i = 0; i < cb_state_ptr->capture_ptr->size; i++) {
      if (col_i == cb_state_ptr->capture_ptr->indexes_ptr[i]) {
        return true;
      }
    }
    return false;
  }
}

static void ensure_row_buffer_space(struct row_buffer *row_buffer_ptr)
{
  struct cell *new_cells_ptr;
  unsigned new_allocated_n;
  unsigned i;

  if (row_buffer_ptr->cols_used == row_buffer_ptr->allocated_n) {
    new_allocated_n = row_buffer_ptr->allocated_n + 5;
    new_cells_ptr = enif_alloc(sizeof(struct cell) * new_allocated_n);
    memcpy(new_cells_ptr,
           row_buffer_ptr->cells_ptr,
           sizeof(struct cell) * row_buffer_ptr->allocated_n);
    for (i = row_buffer_ptr->allocated_n; i < new_allocated_n; i++) {
      new_cells_ptr[i] = empty_cell();
    }
    enif_free(row_buffer_ptr->cells_ptr);
    row_buffer_ptr->cells_ptr = new_cells_ptr;
    row_buffer_ptr->allocated_n = new_allocated_n;
  }
}

static void add_cell(void *data_ptr, size_t size,
                     struct callback_state* cb_state_ptr)
{
  struct row_buffer *row_buffer_ptr = cb_state_ptr->row_buffer_ptr;
  struct cell *cell_ptr;

  ensure_row_buffer_space(row_buffer_ptr);
  if (is_output_column(cb_state_ptr, row_buffer_ptr->cols_used)) {
    cell_ptr = &row_buffer_ptr->cells_ptr[row_buffer_ptr->cols_used];
    ensure_cell_size(cell_ptr, size);
    cell_ptr->data_size = size;
    memcpy(cell_ptr->data_ptr, data_ptr, size);
  }
  row_buffer_ptr->cols_used++;
}

ERL_NIF_TERM make_output_binary(ErlNifEnv* env_ptr, char *data_ptr, size_t size)
{
  ERL_NIF_TERM out_term;
  unsigned char *out_term_data_ptr;

  out_term_data_ptr = enif_make_new_binary(env_ptr, size, &out_term);
  memcpy(out_term_data_ptr, data_ptr, size);
  return out_term;
}

ERL_NIF_TERM make_output_term(ErlNifEnv* env_ptr, char *data_ptr,
                              size_t size, unsigned options)
{
  if (options & OPTION_RETURN_BINARY) {
    return make_output_binary(env_ptr, data_ptr, size);
  } else {
    return enif_make_string_len(env_ptr,
                                data_ptr,
                                size,
                                ERL_NIF_LATIN1);
  }
}

static unsigned make_output_terms(struct callback_state* cb_state_ptr,
                                  ERL_NIF_TERM *out_terms_ptr)
{
  ErlNifEnv* env_ptr = cb_state_ptr->env_ptr;
  struct row_buffer *row_buffer_ptr = cb_state_ptr->row_buffer_ptr;
  struct capture *capture_ptr = cb_state_ptr->capture_ptr;
  unsigned cols_used = row_buffer_ptr->cols_used;
  unsigned out_i;
  unsigned capture_i;

  if (capture_ptr->indexes_ptr == NULL) {
    for (out_i = 0; out_i < cols_used; out_i++) {
      out_terms_ptr[out_i] =
        make_output_term(env_ptr,
                         row_buffer_ptr->cells_ptr[out_i].data_ptr,
                         row_buffer_ptr->cells_ptr[out_i].data_size,
                         cb_state_ptr->options);
    }
  } else {
    for (out_i = 0; out_i < capture_ptr->size; out_i++) {
      capture_i = capture_ptr->indexes_ptr[out_i];
      if (capture_i < row_buffer_ptr->cols_used) {
        out_terms_ptr[out_i] =
          make_output_term(env_ptr,
                           row_buffer_ptr->cells_ptr[capture_i].data_ptr,
                           row_buffer_ptr->cells_ptr[capture_i].data_size,
                           cb_state_ptr->options);
      } else {
        out_terms_ptr[out_i] =
          make_output_term(env_ptr,
                           NULL,
                           0,
                           cb_state_ptr->options);
      }
    }
  }
  return out_i;
}

static unsigned output_row_length(struct callback_state* cb_state_ptr)
{
  struct capture *capture_ptr = cb_state_ptr->capture_ptr;

  if (capture_ptr->indexes_ptr != NULL) {
    return capture_ptr->size;
  } else {
    return cb_state_ptr->row_buffer_ptr->cols_used;
  }
}

static void add_row(struct callback_state* cb_state_ptr)
{
  struct out_buffer *out_buffer_ptr = &(cb_state_ptr->out_buffer);
  struct row_buffer *row_buffer_ptr = cb_state_ptr->row_buffer_ptr;
  unsigned row_length;
  ErlNifEnv* env_ptr = cb_state_ptr->env_ptr;
  ERL_NIF_TERM *out_cells_ptr;
  unsigned out_cells_used;

  assert(out_buffer_ptr->row_n < MAX_ROWS_PER_BATCH);

  row_length = output_row_length(cb_state_ptr);
  out_cells_ptr = enif_alloc(row_length * sizeof(ERL_NIF_TERM));

  out_cells_used = make_output_terms(cb_state_ptr, out_cells_ptr);
  assert(out_cells_used == row_length);
  row_buffer_ptr->cols_used = 0;
  out_buffer_ptr->rows[out_buffer_ptr->row_n] =
    enif_make_list_from_array(env_ptr, out_cells_ptr, out_cells_used);
  out_buffer_ptr->row_n++;

  enif_free(out_cells_ptr);
}

static void column_callback (void *data_ptr, size_t size,
                             void *cb_state_void_ptr)
{
  add_cell(data_ptr,
           size,
           (struct callback_state*) cb_state_void_ptr);
}

static void row_callback (int c, void *cb_state_void_ptr)
{
  add_row((struct callback_state*) cb_state_void_ptr);
}

static ERL_NIF_TERM make_output(struct callback_state *cb_state_ptr)
{
  ERL_NIF_TERM ret;
  ErlNifEnv* env_ptr = cb_state_ptr->env_ptr;
  struct out_buffer out_buffer = cb_state_ptr->out_buffer;

  ret = enif_make_list_from_array(env_ptr, out_buffer.rows, out_buffer.row_n);
  out_buffer.row_n = 0;
  return ret;
}

static void init_row_buffer(struct row_buffer *row_buffer_ptr)
{
  row_buffer_ptr->cells_ptr = NULL;
  row_buffer_ptr->allocated_n = 0;
  row_buffer_ptr->cols_used = 0;
}

static void init_csv_buffer(struct csv_buffer *csv_buffer_ptr)
{
  csv_buffer_ptr->data_ptr = NULL;
  csv_buffer_ptr->size = 0;
}

static void init_capture(struct capture *capture_ptr) {
  capture_ptr->indexes_ptr = NULL;
}

static struct state* init_state(unsigned options)
{
  struct state* state_ptr = enif_alloc_resource(state_type,
                                                sizeof(struct state));
  if (state_ptr != NULL) {
    state_ptr->options = options;
    init_row_buffer(&state_ptr->row_buffer);
    init_csv_buffer(&state_ptr->csv_buffer);
    init_capture(&state_ptr->capture);
  }
  return state_ptr;
}

static bool extract_capture_list(ErlNifEnv* env_ptr, ERL_NIF_TERM list,
                                 unsigned len, unsigned *dst_ptr)
{
  unsigned i;
  ERL_NIF_TERM head;
  ERL_NIF_TERM tail;
  unsigned item;

  tail = list;
  for (i = 0; i < len; i++) {
    if (!enif_get_list_cell(env_ptr, tail, &head, &tail)) {
      return false;
    }
    if (!enif_get_uint(env_ptr, head, &item)) {
      return false;
    }
    dst_ptr[i] = item;
  }
  return true;
}

static bool update_capture(ErlNifEnv* env_ptr, struct state *state_ptr,
                           ERL_NIF_TERM list)
{
  unsigned len;
  unsigned *indexes_new_ptr;
  struct capture *capture_ptr;

  if (!enif_get_list_length(env_ptr, list, &len)) {
    return false;
  } else {
    indexes_new_ptr = enif_alloc(len * sizeof(unsigned));
    if (!extract_capture_list(env_ptr, list, len, indexes_new_ptr)) {
      enif_free(indexes_new_ptr);
      return false;
    } else {
      capture_ptr = &(state_ptr->capture);
      enif_free(capture_ptr->indexes_ptr);
      capture_ptr->indexes_ptr = indexes_new_ptr;
      capture_ptr->size = len;
      return true;
    }
  }
}

static void init_callback_state(struct callback_state *cb_state_ptr,
                                ErlNifEnv* env_ptr, struct state *state_ptr)
{
  cb_state_ptr->env_ptr = env_ptr;
  cb_state_ptr->out_buffer.row_n = 0;
  cb_state_ptr->row_buffer_ptr = &(state_ptr->row_buffer);
  cb_state_ptr->capture_ptr = &(state_ptr->capture);
  cb_state_ptr->options = state_ptr->options;
}

static bool is_csv_buffer_empty(struct csv_buffer *csv_buffer_ptr)
{
  if (csv_buffer_ptr->data_ptr == NULL ||
      csv_buffer_ptr->size == 0 ||
      csv_buffer_ptr->consumed >= csv_buffer_ptr->size) {
    return true;
  } else {
    return false;
  }
}

static void get_csv_chunk(struct csv_chunk *chunk_ptr, struct state *state_ptr,
                          size_t max_len)
{
  struct csv_buffer *csv_buffer_ptr;
  size_t bytes_read;

  csv_buffer_ptr = &state_ptr->csv_buffer;
  if (is_csv_buffer_empty(csv_buffer_ptr)) {
    chunk_ptr->size = 0;
  } else {
    bytes_read = min(csv_buffer_ptr->size - csv_buffer_ptr->consumed,
                     max_len);
    chunk_ptr->size = bytes_read;
    chunk_ptr->data_ptr = csv_buffer_ptr->data_ptr + csv_buffer_ptr->consumed;
    csv_buffer_ptr->consumed += bytes_read;
  }
}

static ERL_NIF_TERM ok_tuple(ErlNifEnv* env_ptr, ERL_NIF_TERM term)
{
  ERL_NIF_TERM ok_atom = enif_make_atom(env_ptr, "ok");

  return enif_make_tuple2(env_ptr, ok_atom, term);
}

static ERL_NIF_TERM error_tuple(ErlNifEnv* env_ptr, ERL_NIF_TERM term)
{
  ERL_NIF_TERM error_atom = enif_make_atom(env_ptr, "error");

  return enif_make_tuple2(env_ptr, error_atom, term);
}

static ERL_NIF_TERM error(ErlNifEnv* env_ptr, const char* reason)
{
  return error_tuple(env_ptr, enif_make_string(env_ptr,
                                               reason,
                                               ERL_NIF_LATIN1));
}

static ERL_NIF_TERM error2(ErlNifEnv* env_ptr, const char* reason1,
                           const char* reason2)
{
  return error_tuple(env_ptr,
                     enif_make_tuple2(env_ptr,
                                      enif_make_string(env_ptr,
                                                       reason1,
                                                       ERL_NIF_LATIN1),
                                      enif_make_string(env_ptr,
                                                       reason2,
                                                       ERL_NIF_LATIN1)));
}

void set_delimiter(struct csv_parser *parser_ptr, unsigned options) {
  unsigned char delimiter;

  if (options & OPTION_DELIM_TABS) {
    delimiter = CSV_TAB;
  } else {
    delimiter = CSV_COMMA;
  }
  csv_set_delim(parser_ptr, delimiter);
}

static ERL_NIF_TERM init(ErlNifEnv* env_ptr, int argc,
                         const ERL_NIF_TERM argv[])
{
  ERL_NIF_TERM resource;
  struct state* state_ptr;
  struct csv_parser *parser_ptr;
  unsigned options;

  if (argc != 1) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_get_uint(env_ptr, argv[0], &options)) {
    return enif_make_badarg(env_ptr);
  }

  state_ptr = init_state(options);
  if (state_ptr == NULL) {
    return error(env_ptr, "init_state failed");
  }
  parser_ptr = &(state_ptr->parser);
  if (csv_init(parser_ptr, 0) != 0) {
    return error(env_ptr, "csv_init failed");
  }
  set_delimiter(parser_ptr, options);

  resource = enif_make_resource(env_ptr, state_ptr);
  enif_release_resource(state_ptr);
  return ok_tuple(env_ptr, resource);
}

static ERL_NIF_TERM close(ErlNifEnv* env_ptr, int argc,
                          const ERL_NIF_TERM argv[])
{
  struct state *state_ptr;
  struct csv_parser *parser_ptr;
  struct callback_state cb_state;

  if (argc != 1) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_get_resource(env_ptr, argv[0], state_type, (void**) &state_ptr)) {
    return enif_make_badarg(env_ptr);
  }
  parser_ptr = &(state_ptr->parser);

  init_callback_state(&cb_state, env_ptr, state_ptr);
  if (csv_fini(parser_ptr, column_callback, row_callback, &cb_state) != 0) {
    return error(env_ptr, "csv_fini failed");
  } else {
    return ok_tuple(env_ptr, make_output(&cb_state));
  }
}

static ERL_NIF_TERM feed(ErlNifEnv* env_ptr, int argc,
                         const ERL_NIF_TERM argv[])
{
  struct state *state_ptr;
  ErlNifBinary csv_bin;
  struct csv_buffer *csv_buffer_ptr;

  if (argc != 2) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_get_resource(env_ptr, argv[0], state_type, (void**) &state_ptr)) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_inspect_binary(env_ptr, argv[1], &csv_bin)) {
    return enif_make_badarg(env_ptr);
  }

  csv_buffer_ptr = &state_ptr->csv_buffer;
  if (is_csv_buffer_empty(csv_buffer_ptr) == false) {
    return error(env_ptr, "csv buffer not empty");
  } else {
    if (csv_buffer_ptr->data_ptr != NULL) {
      enif_free(csv_buffer_ptr->data_ptr);
    }
    csv_buffer_ptr->data_ptr = enif_alloc(csv_bin.size);
    if (csv_buffer_ptr->data_ptr == NULL) {
      return error(env_ptr, "could not allocate csv buffer");
    } else {
      memcpy(csv_buffer_ptr->data_ptr, csv_bin.data, csv_bin.size);
      csv_buffer_ptr->size = csv_bin.size;
      csv_buffer_ptr->consumed = 0;
      return enif_make_atom(env_ptr, "ok");
    }
  }
}

static ERL_NIF_TERM set_capture(ErlNifEnv* env_ptr, int argc,
                                const ERL_NIF_TERM argv[])
{
  struct state *state_ptr;

  if (argc != 2) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_get_resource(env_ptr, argv[0], state_type, (void**) &state_ptr)) {
    return enif_make_badarg(env_ptr);
  }
  if (!update_capture(env_ptr, state_ptr, argv[1])) {
    return enif_make_badarg(env_ptr);
  }

  return enif_make_atom(env_ptr, "ok");
}

static ERL_NIF_TERM parse_one_row(ErlNifEnv* env_ptr, int argc,
                                  const ERL_NIF_TERM argv[])
{
  struct state *state_ptr;
  struct csv_parser *parser_ptr;
  struct callback_state cb_state;
  struct csv_chunk chunk;

  if (argc != 1) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_get_resource(env_ptr, argv[0], state_type, (void**) &state_ptr)) {
    return enif_make_badarg(env_ptr);
  }
  parser_ptr = &(state_ptr->parser);
  init_callback_state(&cb_state, env_ptr, state_ptr);

  while (cb_state.out_buffer.row_n == 0) {
    get_csv_chunk(&chunk, state_ptr, 1);
    if (chunk.size == 0) {
      return enif_make_tuple2(env_ptr,
                              enif_make_atom(env_ptr, "error"),
                              enif_make_atom(env_ptr, "eob"));
    } else {
      if (csv_parse(parser_ptr, chunk.data_ptr, chunk.size,
                    column_callback, row_callback, &cb_state) != chunk.size) {
        return error2(env_ptr,
                      "csv_parse failed",
                      csv_strerror(csv_error(parser_ptr)));
      }
    }
  }
  return ok_tuple(env_ptr, make_output(&cb_state));
}

static ERL_NIF_TERM parse(ErlNifEnv* env_ptr, int argc,
                          const ERL_NIF_TERM argv[])
{
  struct state *state_ptr;
  struct csv_parser *parser_ptr;
  struct callback_state cb_state;
  struct csv_chunk chunk;

  if (argc != 1) {
    return enif_make_badarg(env_ptr);
  }
  if (!enif_get_resource(env_ptr, argv[0], state_type, (void**) &state_ptr)) {
    return enif_make_badarg(env_ptr);
  }
  parser_ptr = &(state_ptr->parser);

  get_csv_chunk(&chunk, state_ptr, MAX_PARSE_SIZE);
  if (chunk.size == 0) {
    return enif_make_tuple2(env_ptr,
                            enif_make_atom(env_ptr, "error"),
                            enif_make_atom(env_ptr, "eob"));
  } else {
    init_callback_state(&cb_state, env_ptr, state_ptr);
    if (csv_parse(parser_ptr, chunk.data_ptr, chunk.size,
                  column_callback, row_callback, &cb_state) != chunk.size) {
      return error2(env_ptr,
                    "csv_parse failed",
                    csv_strerror(csv_error(parser_ptr)));
    } else {
      return ok_tuple(env_ptr, make_output(&cb_state));
    }
  }
}

static ErlNifFunc nif_funcs[] =
  {
    {"init", 1, init},
    {"close", 1, close},
    {"feed", 2, feed},
    {"set_capture", 2, set_capture},
    {"parse_one_row", 1, parse_one_row},
    {"parse", 1, parse},
  };

static void csv_buffer_dtor(struct csv_buffer *csv_buffer_ptr)
{
  if (csv_buffer_ptr->data_ptr != NULL) {
    enif_free(csv_buffer_ptr->data_ptr);
  }
}

static void cell_dtor(struct cell *cell_ptr)
{
  if (cell_ptr->data_ptr != NULL) {
    enif_free(cell_ptr->data_ptr);
  }
}

static void row_buffer_dtor(struct row_buffer *row_buffer_ptr)
{
  unsigned i;

  for (i = 0; i < row_buffer_ptr->allocated_n; i++) {
    cell_dtor(&row_buffer_ptr->cells_ptr[i]);
  }
  enif_free(row_buffer_ptr->cells_ptr);
}

static void capture_dtor(struct capture *capture_ptr)
{
  if (capture_ptr->indexes_ptr != NULL) {
    enif_free(capture_ptr->indexes_ptr);
  }
}

static void state_dtor(ErlNifEnv* env_ptr, void* obj_ptr)
{
  struct state* state_ptr = (struct state*) obj_ptr;
  struct csv_parser *parser_ptr = &(state_ptr->parser);
  struct csv_buffer *csv_buffer_ptr = &(state_ptr->csv_buffer);
  struct row_buffer *row_buffer_ptr = &(state_ptr->row_buffer);
  struct capture *capture_ptr = &(state_ptr->capture);

  csv_free(parser_ptr);
  csv_buffer_dtor(csv_buffer_ptr);
  row_buffer_dtor(row_buffer_ptr);
  capture_dtor(capture_ptr);
}

static int load(ErlNifEnv* env_ptr, void** priv, ERL_NIF_TERM info)
{
  // Use ERL_NIF_RT_TAKEOVER?
  int flags = ERL_NIF_RT_CREATE;
  state_type = enif_open_resource_type(env_ptr, NULL, "state",
                                       state_dtor, flags, NULL);

  if (state_type == NULL) {
    return 1;
  } else {
    return 0;
  }
}

ERL_NIF_INIT(csv_parser, nif_funcs, load, NULL, NULL, NULL)
