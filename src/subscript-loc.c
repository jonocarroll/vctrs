#include "vctrs.h"
#include "utils.h"
#include "subscript-loc.h"

static SEXP int_invert_location(SEXP subscript, R_len_t n);
static SEXP int_filter_zero(SEXP subscript, R_len_t n_zero);

static void stop_subscript_oob_location(SEXP i, R_len_t size);
static void stop_subscript_oob_name(SEXP i, SEXP names);


static SEXP int_as_location(SEXP subscript, R_len_t n,
                            struct vec_as_location_options* opts) {
  const int* data = INTEGER_RO(subscript);
  R_len_t loc_n = Rf_length(subscript);

  // Zeros need to be filtered out from the subscript vector.
  // `int_invert_location()` filters them out for negative indices, but
  // positive indices need to go through and `int_filter_zero()`.
  R_len_t n_zero = 0;

  bool convert_negative = opts->convert_negative;

  for (R_len_t i = 0; i < loc_n; ++i, ++data) {
    int elt = *data;
    if (convert_negative && elt < 0 && elt != NA_INTEGER) {
      return int_invert_location(subscript, n);
    }
    if (elt == 0) {
      ++n_zero;
    }
    if (abs(elt) > n) {
      stop_subscript_oob_location(subscript, n);
    }
  }

  if (n_zero) {
    return int_filter_zero(subscript, n_zero);
  } else {
    return subscript;
  }
}


static SEXP lgl_as_location(SEXP subscript, R_len_t n);

static SEXP int_invert_location(SEXP subscript, R_len_t n) {
  const int* data = INTEGER_RO(subscript);
  R_len_t loc_n = Rf_length(subscript);

  SEXP sel = PROTECT(Rf_allocVector(LGLSXP, n));
  r_lgl_fill(sel, 1, n);

  int* sel_data = LOGICAL(sel);

  for (R_len_t i = 0; i < loc_n; ++i, ++data) {
    int j = *data;

    if (j == NA_INTEGER) {
      Rf_errorcall(R_NilValue, "Can't subset with a mix of negative indices and missing values");
    }
    if (j >= 0) {
      if (j == 0) {
        continue;
      } else {
        Rf_errorcall(R_NilValue, "Can't subset with a mix of negative and positive indices");
      }
    }

    j = -j;
    if (j > n) {
      stop_subscript_oob_location(subscript, n);
    }

    sel_data[j - 1] = 0;
  }

  SEXP out = lgl_as_location(sel, n);

  UNPROTECT(1);
  return out;
}

static SEXP int_filter_zero(SEXP subscript, R_len_t n_zero) {
  R_len_t loc_n = vec_size(subscript);
  const int* data = INTEGER_RO(subscript);

  SEXP out = PROTECT(Rf_allocVector(INTSXP, loc_n - n_zero));
  int* out_data = INTEGER(out);

  for (R_len_t i = 0; i < loc_n; ++i, ++data) {
    int elt = *data;
    if (elt != 0) {
      *out_data = elt;
      ++out_data;
    }
  }

  UNPROTECT(1);
  return out;
}

static SEXP dbl_as_location(SEXP subscript, R_len_t n, struct vec_as_location_options* opts) {
  subscript = PROTECT(vec_cast(subscript, vctrs_shared_empty_int, args_empty, args_empty));
  subscript = int_as_location(subscript, n, opts);

  UNPROTECT(1);
  return subscript;
}

static SEXP lgl_as_location(SEXP subscript, R_len_t n) {
  R_len_t subscript_n = Rf_length(subscript);

  if (subscript_n == n) {
    SEXP out = PROTECT(r_lgl_which(subscript, true));

    SEXP nms = PROTECT(r_names(subscript));
    if (nms != R_NilValue) {
      nms = PROTECT(vec_slice(nms, out));
      r_poke_names(out, nms);
      UNPROTECT(1);
    }

    UNPROTECT(2);
    return out;
  }

  /* A single `TRUE` or `FALSE` index is recycled_nms to the full vector
   * size. This means `TRUE` is synonym for missing index (subscript.e. no
   * subsetting) and `FALSE` is synonym for empty index.
   *
   * We could return the missing argument as sentinel to avoid
   * materialising the index vector for the `TRUE` case but this would
   * make `vec_as_location()` an option type just to optimise a rather
   * uncommon case.
   */
  if (subscript_n == 1) {
    int elt = LOGICAL(subscript)[0];

    SEXP out;
    if (elt == NA_LOGICAL) {
      out = PROTECT(Rf_allocVector(INTSXP, n));
      r_int_fill(out, NA_INTEGER, n);
    } else if (elt) {
      out = PROTECT(Rf_allocVector(INTSXP, n));
      r_int_fill_seq(out, 1, n);
    } else {
      return vctrs_shared_empty_int;
    }

    SEXP nms = PROTECT(r_names(subscript));
    if (nms != R_NilValue) {
      SEXP recycled_nms = PROTECT(Rf_allocVector(STRSXP, n));
      r_chr_fill(recycled_nms, r_chr_get(nms, 0), n);
      r_poke_names(out, recycled_nms);
      UNPROTECT(1);
    }

    UNPROTECT(2);
    return out;
  }

  Rf_errorcall(R_NilValue,
               "Logical indices must have length 1 or be as long as the indexed vector.\n"
               "The vector has size %d whereas the subscript has size %d.",
               n, subscript_n);
}

static SEXP chr_as_location(SEXP subscript, SEXP names) {
  if (names == R_NilValue) {
    Rf_errorcall(R_NilValue, "Can't use character to index an unnamed vector.");
  }

  if (TYPEOF(names) != STRSXP) {
    Rf_errorcall(R_NilValue, "`names` must be a character vector.");
  }

  SEXP matched = PROTECT(Rf_match(names, subscript, NA_INTEGER));

  R_len_t n = Rf_length(matched);
  const int* p = INTEGER_RO(matched);
  const SEXP* ip = STRING_PTR_RO(subscript);

  for (R_len_t k = 0; k < n; ++k) {
    if (p[k] == NA_INTEGER && ip[k] != NA_STRING) {
      stop_subscript_oob_name(subscript, names);
    }
  }

  r_poke_names(matched, PROTECT(r_names(subscript))); UNPROTECT(1);

  UNPROTECT(1);
  return matched;
}

SEXP vec_as_location(SEXP subscript, R_len_t n, SEXP names) {
  struct vec_as_location_options opts = { .convert_negative = true };
  return vec_as_location_opts(subscript, n, names, &opts);
}

SEXP vec_as_location_opts(SEXP subscript, R_len_t n, SEXP names,
                          struct vec_as_location_options* opts) {

  if (vec_dim_n(subscript) != 1) {
    Rf_errorcall(R_NilValue, "`i` must have one dimension, not %d.", vec_dim_n(subscript));
  }

  switch (TYPEOF(subscript)) {
  case NILSXP: return vctrs_shared_empty_int;
  case INTSXP: return int_as_location(subscript, n, opts);
  case REALSXP: return dbl_as_location(subscript, n, opts);
  case LGLSXP: return lgl_as_location(subscript, n);
  case STRSXP: return chr_as_location(subscript, names);

  default: Rf_errorcall(R_NilValue, "`i` must be an integer, character, or logical vector, not a %s.",
                        Rf_type2char(TYPEOF(subscript)));
  }
}

SEXP vctrs_as_location(SEXP subscript, SEXP n, SEXP names, SEXP convert_negative) {
  if (!r_is_bool(convert_negative)) {
    Rf_error("Internal error: `convert_negative` must be a boolean");
  }
  struct vec_as_location_options opts = { .convert_negative = LOGICAL(convert_negative)[0]};
  return vec_as_location_opts(subscript, r_int_get(n, 0), names, &opts);
}


static void stop_subscript_oob_location(SEXP i, R_len_t size) {
  SEXP size_obj = PROTECT(r_int(size));
  vctrs_eval_mask2(Rf_install("stop_subscript_oob_location"),
                   syms_i, i,
                   syms_size, size_obj,
                   vctrs_ns_env);

  UNPROTECT(1);
  never_reached("stop_subscript_oob_location");
}
static void stop_subscript_oob_name(SEXP i, SEXP names) {
  vctrs_eval_mask2(Rf_install("stop_subscript_oob_name"),
                   syms_i, i,
                   syms_names, names,
                   vctrs_ns_env);

  UNPROTECT(1);
  never_reached("stop_subscript_oob_name");
}