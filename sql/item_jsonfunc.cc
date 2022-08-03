/* Copyright (c) 2016, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "item.h"
#include "sql_parse.h" // For check_stack_overrun

/*
  Allocating memory and *also* using it (reading and
  writing from it) because some build instructions cause
  compiler to optimize out stack_used_up. Since alloca()
  here depends on stack_used_up, it doesnt get executed
  correctly and causes json_debug_nonembedded to fail
  ( --error ER_STACK_OVERRUN_NEED_MORE does not occur).
*/
#define ALLOCATE_MEM_ON_STACK(A) do \
                              { \
                                uchar *array= (uchar*)alloca(A); \
                                bzero(array, A); \
                                my_checksum(0, array, A); \
                              } while(0)

/*
  Compare ASCII string against the string with the specified
  character set.
  Only compares the equality, case insensitive.
*/
static bool eq_ascii_string(const CHARSET_INFO *cs,
                            const char *ascii,
                            const char *s,  uint32 s_len)
{
  const char *s_end= s + s_len;

  while (*ascii && s < s_end)
  {
    my_wc_t wc;
    int wc_len;

    wc_len= cs->mb_wc(&wc, (uchar *) s, (uchar *) s_end);
    if (wc_len <= 0 || (wc | 0x20) != (my_wc_t) *ascii)
      return 0;

    ascii++;
    s+= wc_len;
  }

  return *ascii == 0 && s >= s_end;
}


static bool append_simple(String *s, const char *a, size_t a_len)
{
  if (!s->realloc_with_extra_if_needed(s->length() + a_len))
  {
    s->q_append(a, a_len);
    return FALSE;
  }

  return TRUE;
}


static inline bool append_simple(String *s, const uchar *a, size_t a_len)
{
  return append_simple(s, (const char *) a, a_len);
}


/*
  Appends JSON string to the String object taking charsets in
  consideration.
*/
static int st_append_json(String *s,
             CHARSET_INFO *json_cs, const uchar *js, uint js_len)
{
  int str_len= js_len * s->charset()->mbmaxlen;

  if (!s->reserve(str_len, 1024) &&
      (str_len= json_unescape(json_cs, js, js + js_len,
         s->charset(), (uchar *) s->end(), (uchar *) s->end() + str_len)) > 0)
  {
    s->length(s->length() + str_len);
    return 0;
  }

  return str_len;
}


/*
  Appends arbitrary String to the JSON string taking charsets in
  consideration.
*/
static int st_append_escaped(String *s, const String *a)
{
  /*
    In the worst case one character from the 'a' string
    turns into '\uXXXX\uXXXX' which is 12.
  */
  int str_len= a->length() * 12 * s->charset()->mbmaxlen /
               a->charset()->mbminlen;
  if (!s->reserve(str_len, 1024) &&
      (str_len=
         json_escape(a->charset(), (uchar *) a->ptr(), (uchar *)a->end(),
                     s->charset(),
                     (uchar *) s->end(), (uchar *)s->end() + str_len)) > 0)
  {
    s->length(s->length() + str_len);
    return 0;
  }

  return a->length();
}


static const int TAB_SIZE_LIMIT= 8;
static const char tab_arr[TAB_SIZE_LIMIT+1]= "        ";

static int append_tab(String *js, int depth, int tab_size)
{
  if (js->append('\n'))
    return 1;
  for (int i=0; i<depth; i++)
  {
    if (js->append(tab_arr, tab_size))
      return 1;
  }
  return 0;
}

int json_path_parts_compare(
    const json_path_step_t *a, const json_path_step_t *a_end,
    const json_path_step_t *b, const json_path_step_t *b_end,
    enum json_value_types vt, const int *array_sizes)
{
  int res, res2;
  const json_path_step_t *temp_b= b;

  while (a <= a_end)
  {
    if (b > b_end)
    {
      while (vt != JSON_VALUE_ARRAY &&
             (a->type & JSON_PATH_ARRAY_WILD) == JSON_PATH_ARRAY &&
             a->n_item == 0)
      {
        if (++a > a_end)
          return 0;
      }
      return -2;
    }

    DBUG_ASSERT((b->type & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD)) == 0);

    if (a->type & JSON_PATH_ARRAY)
    {
      if (b->type & JSON_PATH_ARRAY)
      {
        int res= 0, corrected_n_item_a= 0;
        if (array_sizes)
          corrected_n_item_a= a->n_item < 0 ?
                                array_sizes[b-temp_b] + a->n_item : a->n_item;
        if (a->type & JSON_PATH_ARRAY_RANGE)
        {
          int corrected_n_item_end_a= 0;
          if (array_sizes)
            corrected_n_item_end_a= a->n_item_end < 0 ?
                                    array_sizes[b-temp_b] + a->n_item_end :
                                    a->n_item_end;
          res= b->n_item >= corrected_n_item_a &&
                b->n_item <= corrected_n_item_end_a;
        }
        else
         res= corrected_n_item_a == b->n_item;

        if ((a->type & JSON_PATH_WILD) || res)
          goto step_fits;
        goto step_failed;
      }
      if ((a->type & JSON_PATH_WILD) == 0 && a->n_item == 0)
        goto step_fits_autowrap;
      goto step_failed;
    }
    else /* JSON_PATH_KEY */
    {
      if (!(b->type & JSON_PATH_KEY))
        goto step_failed;

      if (!(a->type & JSON_PATH_WILD) &&
          (a->key_end - a->key != b->key_end - b->key ||
           memcmp(a->key, b->key, a->key_end - a->key) != 0))
        goto step_failed;

      goto step_fits;
    }
step_failed:
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
      return -1;
    b++;
    continue;

step_fits:
    b++;
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
    {
      a++;
      continue;
    }

    /* Double wild handling needs recursions. */
    res= json_path_parts_compare(a+1, a_end, b, b_end, vt,
                                 array_sizes ? array_sizes + (b - temp_b) :
                                               NULL);
    if (res == 0)
      return 0;

    res2= json_path_parts_compare(a, a_end, b, b_end, vt,
                                  array_sizes ? array_sizes + (b - temp_b) :
                                                NULL);

    return (res2 >= 0) ? res2 : res;

step_fits_autowrap:
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
    {
      a++;
      continue;
    }

    /* Double wild handling needs recursions. */
    res= json_path_parts_compare(a+1, a_end, b+1, b_end, vt,
                                 array_sizes ? array_sizes + (b - temp_b) :
                                               NULL);
    if (res == 0)
      return 0;

    res2= json_path_parts_compare(a, a_end, b+1, b_end, vt,
                                  array_sizes ? array_sizes + (b - temp_b) :
                                                NULL);

    return (res2 >= 0) ? res2 : res;

  }

  return b <= b_end;
}


int json_path_compare(const json_path_t *a, const json_path_t *b,
                      enum json_value_types vt, const int *array_size)
{
  return json_path_parts_compare(a->steps+1, a->last_step,
                                 b->steps+1, b->last_step, vt, array_size);
}


static int json_nice(json_engine_t *je, String *nice_js,
                     Item_func_json_format::formats mode, int tab_size=4)
{
  int depth= 0;
  static const char *comma= ", ", *colon= "\": ";
  uint comma_len, colon_len;
  int first_value= 1;

  nice_js->length(0);
  nice_js->set_charset(je->s.cs);
  nice_js->alloc(je->s.str_end - je->s.c_str + 32);

  DBUG_ASSERT(mode != Item_func_json_format::DETAILED ||
              (tab_size >= 0 && tab_size <= TAB_SIZE_LIMIT));

  if (mode == Item_func_json_format::LOOSE)
  {
    comma_len= 2;
    colon_len= 3;
  }
  else if (mode == Item_func_json_format::DETAILED)
  {
    comma_len= 1;
    colon_len= 3;
  }
  else
  {
    comma_len= 1;
    colon_len= 2;
  }

  do
  {
    switch (je->state)
    {
    case JST_KEY:
      {
        const uchar *key_start= je->s.c_str;
        const uchar *key_end;

        do
        {
          key_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);
        
        if (unlikely(je->s.error))
          goto error;

        if (!first_value)
          nice_js->append(comma, comma_len);

        if (mode == Item_func_json_format::DETAILED &&
            append_tab(nice_js, depth, tab_size))
          goto error;

        nice_js->append('"');
        append_simple(nice_js, key_start, key_end - key_start);
        nice_js->append(colon, colon_len);
      }
      /* now we have key value to handle, so no 'break'. */
      DBUG_ASSERT(je->state == JST_VALUE);
      goto handle_value;

    case JST_VALUE:
      if (!first_value)
        nice_js->append(comma, comma_len);

      if (mode == Item_func_json_format::DETAILED &&
          depth > 0 &&
          append_tab(nice_js, depth, tab_size))
        goto error;

handle_value:
      if (json_read_value(je))
        goto error;
      if (json_value_scalar(je))
      {
        if (append_simple(nice_js, je->value_begin,
                          je->value_end - je->value_begin))
          goto error;

        first_value= 0;
      }
      else
      {
        if (mode == Item_func_json_format::DETAILED &&
            depth > 0 &&
            append_tab(nice_js, depth, tab_size))
          goto error;
        nice_js->append((je->value_type == JSON_VALUE_OBJECT) ? "{" : "[", 1);
        first_value= 1;
        depth++;
      }

      break;

    case JST_OBJ_END:
    case JST_ARRAY_END:
      depth--;
      if (mode == Item_func_json_format::DETAILED &&
          append_tab(nice_js, depth, tab_size))
        goto error;
      nice_js->append((je->state == JST_OBJ_END) ? "}": "]", 1);
      first_value= 0;
      break;

    default:
      break;
    };
  } while (json_scan_next(je) == 0);

  return je->s.error || *je->killed_ptr;

error:
  return 1;
}


#define report_json_error(js, je, n_param) \
  report_json_error_ex(js->ptr(), je, func_name(), n_param, \
      Sql_condition::WARN_LEVEL_WARN)

void report_json_error_ex(const char *js, json_engine_t *je,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv)
{
  THD *thd= current_thd;
  int position= (int)((const char *) je->s.c_str - js);
  uint code;

  n_param++;

  switch (je->s.error)
  {
  case JE_BAD_CHR:
    code= ER_JSON_BAD_CHR;
    break;

  case JE_NOT_JSON_CHR:
    code= ER_JSON_NOT_JSON_CHR;
    break;

  case JE_EOS:
    code= ER_JSON_EOS;
    break;

  case JE_SYN:
  case JE_STRING_CONST:
    code= ER_JSON_SYNTAX;
    break;

  case JE_ESCAPING:
    code= ER_JSON_ESCAPING;
    break;

  case JE_DEPTH:
    code= ER_JSON_DEPTH;
    if (lv == Sql_condition::WARN_LEVEL_ERROR)
      my_error(code, MYF(0), JSON_DEPTH_LIMIT, n_param, fname, position);
    else
      push_warning_printf(thd, lv, code, ER_THD(thd, code), JSON_DEPTH_LIMIT,
                          n_param, fname, position);
    return;

  default:
    return;
  }

  if (lv == Sql_condition::WARN_LEVEL_ERROR)
    my_error(code, MYF(0), n_param, fname, position);
  else
    push_warning_printf(thd, lv, code, ER_THD(thd, code),
                        n_param, fname, position);
}



#define NO_WILDCARD_ALLOWED 1
#define SHOULD_END_WITH_ARRAY 2
#define TRIVIAL_PATH_NOT_ALLOWED 3

#define report_path_error(js, je, n_param) \
  report_path_error_ex(js->ptr(), je, func_name(), n_param,\
      Sql_condition::WARN_LEVEL_WARN)

void report_path_error_ex(const char *ps, json_path_t *p,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv)
{
  THD *thd= current_thd;
  int position= (int)((const char *) p->s.c_str - ps + 1);
  uint code;

  n_param++;

  switch (p->s.error)
  {
  case JE_BAD_CHR:
  case JE_NOT_JSON_CHR:
  case JE_SYN:
    code= ER_JSON_PATH_SYNTAX;
    break;

  case JE_EOS:
    code= ER_JSON_PATH_EOS;
    break;

  case JE_DEPTH:
    code= ER_JSON_PATH_DEPTH;
    if (lv == Sql_condition::WARN_LEVEL_ERROR)
      my_error(code, MYF(0), JSON_DEPTH_LIMIT, n_param, fname, position);
    else
      push_warning_printf(thd, lv, code, ER_THD(thd, code),
                          JSON_DEPTH_LIMIT, n_param, fname, position);
    return;

  case NO_WILDCARD_ALLOWED:
    code= ER_JSON_PATH_NO_WILDCARD;
    break;

  case TRIVIAL_PATH_NOT_ALLOWED:
    code= ER_JSON_PATH_EMPTY;
    break;


  default:
    return;
  }
  if (lv == Sql_condition::WARN_LEVEL_ERROR)
    my_error(code, MYF(0), n_param, fname, position);
  else
    push_warning_printf(thd, lv, code, ER_THD(thd, code),
                        n_param, fname, position);
}


/*
  Checks if the path has '.*' '[*]' or '**' constructions
  and sets the NO_WILDCARD_ALLOWED error if the case.
*/
static int path_setup_nwc(json_path_t *p, CHARSET_INFO *i_cs,
                          const uchar *str, const uchar *end)
{
  if (!json_path_setup(p, i_cs, str, end))
  {
    if ((p->types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD |
                          JSON_PATH_ARRAY_RANGE)) == 0)
      return 0;
    p->s.error= NO_WILDCARD_ALLOWED;
  }

  return 1;
}


longlong Item_func_json_valid::val_int()
{
  String *js= args[0]->val_json(&tmp_value);

  if ((null_value= args[0]->null_value))
    return 0;

  return json_valid(js->ptr(), js->length(), js->charset());
}


bool Item_func_json_equals::fix_length_and_dec(THD *thd)
{
  if (Item_bool_func::fix_length_and_dec(thd))
    return TRUE;
  set_maybe_null();
  return FALSE;
}


longlong Item_func_json_equals::val_int()
{
  longlong result= 0;

  String a_tmp, b_tmp;

  String *a= args[0]->val_json(&a_tmp);
  String *b= args[1]->val_json(&b_tmp);

  DYNAMIC_STRING a_res;
  if (init_dynamic_string(&a_res, NULL, 0, 0))
  {
    null_value= 1;
    return 1;
  }

  DYNAMIC_STRING b_res;
  if (init_dynamic_string(&b_res, NULL, 0, 0))
  {
    dynstr_free(&a_res);
    null_value= 1;
    return 1;
  }

  if ((null_value= args[0]->null_value || args[1]->null_value))
  {
    null_value= 1;
    goto end;
  }

  if (json_normalize(&a_res, a->ptr(), a->length(), a->charset()))
  {
    null_value= 1;
    goto end;
  }

  if (json_normalize(&b_res, b->ptr(), b->length(), b->charset()))
  {
    null_value= 1;
    goto end;
  }

  result= strcmp(a_res.str, b_res.str) ? 0 : 1;

end:
  dynstr_free(&b_res);
  dynstr_free(&a_res);
  return result;
}


bool Item_func_json_exists::fix_length_and_dec(THD *thd)
{
  if (Item_bool_func::fix_length_and_dec(thd))
    return TRUE;
  set_maybe_null();
  path.set_constant_flag(args[1]->const_item());
  return FALSE;
}


longlong Item_func_json_exists::val_int()
{
  json_engine_t je;
  int array_counters[JSON_DEPTH_LIMIT];

  String *js= args[0]->val_json(&tmp_js);

  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        json_path_setup(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
      goto err_return;
    path.parsed= path.constant;
  }

  if ((null_value= args[0]->null_value || args[1]->null_value))
  {
    null_value= 1;
    return 0;
  }

  null_value= 0;
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  path.cur_step= path.p.steps;
  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto err_return;
    return 0;
  }

  return 1;

err_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_value::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_constant_flag(args[1]->const_item());
  set_maybe_null();
  return FALSE;
}


bool Item_func_json_query::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_constant_flag(args[1]->const_item());
  set_maybe_null();
  return FALSE;
}


/*
  Returns NULL, not an error if the found value
  is not a scalar.
*/
bool Json_path_extractor::extract(String *str, Item *item_js, Item *item_jp,
                                  CHARSET_INFO *cs)
{
  String *js= item_js->val_json(&tmp_js);
  int error= 0;
  int array_counters[JSON_DEPTH_LIMIT];

  if (!parsed)
  {
    String *s_p= item_jp->val_str(&tmp_path);
    if (s_p &&
        json_path_setup(&p, s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
      return true;
    parsed= constant;
  }

  if (item_js->null_value || item_jp->null_value)
    return true;

  Json_engine_scan je(*js);
  str->length(0);
  str->set_charset(cs);

  cur_step= p.steps;
continue_search:
  if (json_find_path(&je, &p, &cur_step, array_counters))
    return true;

  if (json_read_value(&je))
    return true;

  if (unlikely(check_and_get_value(&je, str, &error)))
  {
    if (error)
      return true;
    goto continue_search;
  }

  return false;
}


bool Json_engine_scan::check_and_get_value_scalar(String *res, int *error)
{
  CHARSET_INFO *json_cs;
  const uchar *js;
  uint js_len;

  if (!json_value_scalar(this))
  {
    /* We only look for scalar values! */
    if (json_skip_level(this) || json_scan_next(this))
      *error= 1;
    return true;
  }

  if (value_type == JSON_VALUE_TRUE ||
      value_type == JSON_VALUE_FALSE)
  {
    json_cs= &my_charset_utf8mb4_bin;
    js= (const uchar *) ((value_type == JSON_VALUE_TRUE) ? "1" : "0");
    js_len= 1;
  }
  else
  {
    json_cs= s.cs;
    js= value;
    js_len= value_len;
  }


  return st_append_json(res, json_cs, js, js_len);
}


bool Json_engine_scan::check_and_get_value_complex(String *res, int *error)
{
  if (json_value_scalar(this))
  {
    /* We skip scalar values. */
    if (json_scan_next(this))
      *error= 1;
    return true;
  }

  const uchar *tmp_value= value;
  if (json_skip_level(this))
  {
    *error= 1;
    return true;
  }

  res->set((const char *) value, (uint32)(s.c_str - tmp_value), s.cs);
  return false;
}


bool Item_func_json_quote::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb4_bin);
  /*
    Odd but realistic worst case is when all characters
    of the argument turn into '\uXXXX\uXXXX', which is 12.
  */
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * 12 + 2);
  return FALSE;
}


String *Item_func_json_quote::val_str(String *str)
{
  String *s= args[0]->val_str(&tmp_s);

  if ((null_value= (args[0]->null_value ||
                    args[0]->result_type() != STRING_RESULT)))
    return NULL;

  str->length(0);
  str->set_charset(&my_charset_utf8mb4_bin);

  if (str->append('"') ||
      st_append_escaped(str, s) ||
      str->append('"'))
  {
    /* Report an error. */
    null_value= 1;
    return 0;
  }

  return str;
}


bool Item_func_json_unquote::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb3_general_ci,
                DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  max_length= args[0]->max_length;
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_unquote::read_json(json_engine_t *je)
{
  String *js= args[0]->val_json(&tmp_s);

  if ((null_value= args[0]->null_value))
    return 0;

  json_scan_start(je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (json_read_value(je))
    goto error;

  return js;

error:
  if (je->value_type == JSON_VALUE_STRING)
    report_json_error(js, je, 0);
  return js;
}


String *Item_func_json_unquote::val_str(String *str)
{
  json_engine_t je;
  int c_len;
  String *js;

  if (!(js= read_json(&je)))
    return NULL;

  if (unlikely(je.s.error) || je.value_type != JSON_VALUE_STRING)
    return js;

  str->length(0);
  str->set_charset(&my_charset_utf8mb3_general_ci);

  if (str->realloc_with_extra_if_needed(je.value_len) ||
      (c_len= json_unescape(js->charset(),
        je.value, je.value + je.value_len,
        &my_charset_utf8mb3_general_ci,
        (uchar *) str->ptr(), (uchar *) (str->ptr() + je.value_len))) < 0)
    goto error;

  str->length(c_len);
  return str;

error:
  report_json_error(js, &je, 0);
  return js;
}


static int alloc_tmp_paths(THD *thd, uint n_paths,
                           json_path_with_flags **paths, String **tmp_paths)
{
  if (n_paths > 0)
  {
    if (*tmp_paths == 0)
    {
      MEM_ROOT *root= thd->stmt_arena->mem_root;

      *paths= (json_path_with_flags *) alloc_root(root,
          sizeof(json_path_with_flags) * n_paths);

      *tmp_paths= new (root) String[n_paths];
      if (*paths == 0 || *tmp_paths == 0)
        return 1;

      for (uint c_path=0; c_path < n_paths; c_path++)
        (*tmp_paths)[c_path].set_charset(&my_charset_utf8mb3_general_ci);
    }

    return 0;
  }

  /* n_paths == 0 */
  *paths= 0;
  *tmp_paths= 0;
  return 0;
}


static void mark_constant_paths(json_path_with_flags *p,
                                Item** args, uint n_args)
{
  uint n;
  for (n= 0; n < n_args; n++)
    p[n].set_constant_flag(args[n]->const_item());
}


bool Item_json_str_multipath::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, get_n_paths(), &paths, &tmp_paths) ||
         Item_str_func::fix_fields(thd, ref);
}


void Item_json_str_multipath::cleanup()
{
  if (tmp_paths)
  {
    for (uint i= get_n_paths(); i>0; i--)
      tmp_paths[i-1].free();
  }
  Item_str_func::cleanup();
}


bool Item_func_json_extract::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length * (arg_count - 1);

  mark_constant_paths(paths, args+1, arg_count-1);
  set_maybe_null();
  return FALSE;
}


static bool path_exact(const json_path_with_flags *paths_list, int n_paths,
                       const json_path_t *p, json_value_types vt,
                       const int *array_size_counter)
{
  for (; n_paths > 0; n_paths--, paths_list++)
  {
    if (json_path_compare(&paths_list->p, p, vt, array_size_counter) == 0)
      return TRUE;
  }
  return FALSE;
}


static bool path_ok(const json_path_with_flags *paths_list, int n_paths,
                    const json_path_t *p, json_value_types vt,
                    const int *array_size_counter)
{
  for (; n_paths > 0; n_paths--, paths_list++)
  {
    if (json_path_compare(&paths_list->p, p, vt, array_size_counter) >= 0)
      return TRUE;
  }
  return FALSE;
}


String *Item_func_json_extract::read_json(String *str,
                                          json_value_types *type,
                                          char **out_val, int *value_len)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, sav_je;
  json_path_t p;
  const uchar *value;
  int not_first_value= 0;
  uint n_arg;
  size_t v_len;
  int possible_multiple_values;
  int array_size_counter[JSON_DEPTH_LIMIT];
  uint has_negative_path= 0;

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    c_path->p.types_used= JSON_PATH_KEY_NULL;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-1));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto return_null;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }

    if (args[n_arg]->null_value)
      goto return_null;
  }

  possible_multiple_values= arg_count > 2 ||
    (paths[0].p.types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD |
                              JSON_PATH_ARRAY_RANGE));

  *type= possible_multiple_values ? JSON_VALUE_ARRAY : JSON_VALUE_NULL;

  if (str)
  {
    str->set_charset(js->charset());
    str->length(0);

    if (possible_multiple_values && str->append('['))
      goto error;
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {
    if (has_negative_path && je.value_type == JSON_VALUE_ARRAY &&
        json_skip_array_and_count(&je,
                                  array_size_counter + (p.last_step - p.steps)))
      goto error;

    if (!path_exact(paths, arg_count-1, &p, je.value_type, array_size_counter))
      continue;

    value= je.value_begin;

    if (*type == JSON_VALUE_NULL)
    {
      *type= je.value_type;
      *out_val= (char *) je.value;
      *value_len= je.value_len;
    }
    if (!str)
    {
      /* If str is NULL, we only care about the first found value. */
      goto return_ok;
    }

    if (json_value_scalar(&je))
      v_len= je.value_end - value;
    else
    {
      if (possible_multiple_values)
        sav_je= je;
      if (json_skip_level(&je))
        goto error;
      v_len= je.s.c_str - value;
      if (possible_multiple_values)
        je= sav_je;
    }

    if ((not_first_value && str->append(", ", 2)) ||
        str->append((const char *) value, v_len))
      goto error; /* Out of memory. */

    not_first_value= 1;

    if (!possible_multiple_values)
    {
      /* Loop to the end of the JSON just to make sure it's valid. */
      while (json_get_path_next(&je, &p) == 0) {}
      break;
    }
  }

  if (unlikely(je.s.error))
    goto error;

  if (!not_first_value)
  {
    /* Nothing was found. */
    goto return_null;
  }

  if (possible_multiple_values && str->append(']'))
    goto error; /* Out of memory. */

  js= str;
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  if (json_nice(&je, &tmp_js, Item_func_json_format::LOOSE))
    goto error;

return_ok:
  return &tmp_js;

error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}


String *Item_func_json_extract::val_str(String *str)
{
  json_value_types type;
  char *value;
  int value_len;
  return read_json(str, &type, &value, &value_len);
}


longlong Item_func_json_extract::val_int()
{
  json_value_types type;
  char *value;
  int value_len;
  longlong i= 0;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_NUMBER:
      case JSON_VALUE_STRING:
      {
        char *end;
        int err;
        i= collation.collation->strntoll(value, value_len, 10, &end, &err);
        break;
      }
      case JSON_VALUE_TRUE:
        i= 1;
        break;
      default:
        i= 0;
        break;
    };
  }
  return i;
}


double Item_func_json_extract::val_real()
{
  json_value_types type;
  char *value;
  int value_len;
  double d= 0.0;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_STRING:
      case JSON_VALUE_NUMBER:
      {
        char *end;
        int err;
        d= collation.collation->strntod(value, value_len, &end, &err);
        break;
      }
      case JSON_VALUE_TRUE:
        d= 1.0;
        break;
      default:
        break;
    };
  }

  return d;
}


my_decimal *Item_func_json_extract::val_decimal(my_decimal *to)
{
  json_value_types type;
  char *value;
  int value_len;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_STRING:
      case JSON_VALUE_NUMBER:
      {
        my_decimal *res= decimal_from_string_with_check(to, collation.collation,
                                                        value,
                                                        value + value_len);
        null_value= res == NULL;
        return res;
      }
      case JSON_VALUE_TRUE:
        int2my_decimal(E_DEC_FATAL_ERROR, 1, false/*unsigned_flag*/, to);
        return to;
      case JSON_VALUE_OBJECT:
      case JSON_VALUE_ARRAY:
      case JSON_VALUE_FALSE:
      case JSON_VALUE_NULL:
      case JSON_VALUE_UNINITIALIZED:
      break;
    };
  }
  int2my_decimal(E_DEC_FATAL_ERROR, 0, false/*unsigned_flag*/, to);
  return to;
}



bool Item_func_json_contains::fix_length_and_dec(THD *thd)
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  set_maybe_null();
  if (arg_count > 2)
    path.set_constant_flag(args[2]->const_item());
  return Item_bool_func::fix_length_and_dec(thd);
}


static int find_key_in_object(json_engine_t *j, json_string_t *key)
{
  const uchar *c_str= key->c_str;

  while (json_scan_next(j) == 0 && j->state != JST_OBJ_END)
  {
    DBUG_ASSERT(j->state == JST_KEY);
    if (json_key_matches(j, key))
      return TRUE;
    if (json_skip_key(j))
      return FALSE;
    key->c_str= c_str;
  }

  return FALSE;
}


static int check_contains(json_engine_t *js, json_engine_t *value)
{
  json_engine_t loc_js;
  bool set_js;
  long arbitrary_var;
  long stack_used_up= (available_stack_size(current_thd->thread_stack, &arbitrary_var));
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  {ALLOCATE_MEM_ON_STACK(my_thread_stack_size-stack_used_up-STACK_MIN_SIZE);});
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  switch (js->value_type)
  {
  case JSON_VALUE_OBJECT:
  {
    json_string_t key_name;

    if (value->value_type != JSON_VALUE_OBJECT)
      return FALSE;

    loc_js= *js;
    set_js= FALSE;
    json_string_set_cs(&key_name, value->s.cs);
    while (json_scan_next(value) == 0 && value->state != JST_OBJ_END)
    {
      const uchar *k_start, *k_end;

      DBUG_ASSERT(value->state == JST_KEY);
      k_start= value->s.c_str;
      do
      {
        k_end= value->s.c_str;
      } while (json_read_keyname_chr(value) == 0);

      if (unlikely(value->s.error) || json_read_value(value))
        return FALSE;

      if (set_js)
        *js= loc_js;
      else
        set_js= TRUE;

      json_string_set_str(&key_name, k_start, k_end);
      if (!find_key_in_object(js, &key_name) ||
          json_read_value(js) ||
          !check_contains(js, value))
        return FALSE;
    }

    return value->state == JST_OBJ_END && !json_skip_level(js);
  }
  case JSON_VALUE_ARRAY:
    if (value->value_type != JSON_VALUE_ARRAY)
    {
      loc_js= *value;
      set_js= FALSE;
      while (json_scan_next(js) == 0 && js->state != JST_ARRAY_END)
      {
        int c_level, v_scalar;
        DBUG_ASSERT(js->state == JST_VALUE);
        if (json_read_value(js))
          return FALSE;

        if (!(v_scalar= json_value_scalar(js)))
          c_level= json_get_level(js);

        if (set_js)
          *value= loc_js;
        else
          set_js= TRUE;

        if (check_contains(js, value))
        {
          if (json_skip_level(js))
            return FALSE;
          return TRUE;
        }
        if (unlikely(value->s.error) || unlikely(js->s.error) ||
            (!v_scalar && json_skip_to_level(js, c_level)))
          return FALSE;
      }
      return FALSE;
    }
    /* else */
    loc_js= *js;
    set_js= FALSE;
    while (json_scan_next(value) == 0 && value->state != JST_ARRAY_END)
    {
      DBUG_ASSERT(value->state == JST_VALUE);
      if (json_read_value(value))
        return FALSE;

      if (set_js)
        *js= loc_js;
      else
        set_js= TRUE;
      if (!check_contains(js, value))
        return FALSE;
    }

    return value->state == JST_ARRAY_END;

  case JSON_VALUE_STRING:
    if (value->value_type != JSON_VALUE_STRING)
      return FALSE;
    /*
       TODO: make proper json-json comparison here that takes excipient
             into account.
     */
    return value->value_len == js->value_len &&
           memcmp(value->value, js->value, value->value_len) == 0;
  case JSON_VALUE_NUMBER:
    if (value->value_type == JSON_VALUE_NUMBER)
    {
      double d_j, d_v;
      char *end;
      int err;

      d_j= js->s.cs->strntod((char *) js->value, js->value_len, &end, &err);;
      d_v= value->s.cs->strntod((char *) value->value, value->value_len, &end, &err);;

      return (fabs(d_j - d_v) < 1e-12);
    }
    else
      return FALSE;

  default:
    break;
  }

  /*
    We have these not mentioned in the 'switch' above:

    case JSON_VALUE_TRUE:
    case JSON_VALUE_FALSE:
    case JSON_VALUE_NULL:
  */
  return value->value_type == js->value_type;
}


longlong Item_func_json_contains::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, ve;
  int result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count>2) /* Path specified. */
  {
    int array_counters[JSON_DEPTH_LIMIT];
    if (!path.parsed)
    {
      String *s_p= args[2]->val_str(&tmp_path);
      if (s_p &&
          path_setup_nwc(&path.p,s_p->charset(),(const uchar *) s_p->ptr(),
                         (const uchar *) s_p->end()))
      {
        report_path_error(s_p, &path.p, 2);
        goto return_null;
      }
      path.parsed= path.constant;
    }
    if (args[2]->null_value)
      goto return_null;

    path.cur_step= path.p.steps;
    if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
    {
      if (je.s.error)
      {
        ve.s.error= 0;
        goto error;
      }

      return FALSE;
    }
  }

  json_scan_start(&ve, val->charset(),(const uchar *) val->ptr(),
                  (const uchar *) val->end());

  if (json_read_value(&je) || json_read_value(&ve))
    goto error;

  result= check_contains(&je, &ve);
  if (unlikely(je.s.error || ve.s.error))
    goto error;

  return result;

error:
  if (je.s.error)
    report_json_error(js, &je, 0);
  if (ve.s.error)
    report_json_error(val, &ve, 1);
return_null:
  null_value= 1;
  return 0;
}


bool Item_func_json_contains_path::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count-2, &paths, &tmp_paths) ||
         (p_found= (bool *) alloc_root(thd->mem_root,
                                       (arg_count-2)*sizeof(bool))) == NULL ||
         Item_int_func::fix_fields(thd, ref);
}


bool Item_func_json_contains_path::fix_length_and_dec(THD *thd)
{
  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;
  set_maybe_null();
  mark_constant_paths(paths, args+2, arg_count-2);
  return Item_bool_func::fix_length_and_dec(thd);
}


void Item_func_json_contains_path::cleanup()
{
  if (tmp_paths)
  {
    for (uint i= arg_count-2; i>0; i--)
      tmp_paths[i-1].free();
    tmp_paths= 0;
  }
  Item_int_func::cleanup();
}


static int parse_one_or_all(const Item_func *f, Item *ooa_arg,
                            bool *ooa_parsed, bool ooa_constant, bool *mode_one)
{
  if (!*ooa_parsed)
  {
    char buff[20];
    String *res, tmp(buff, sizeof(buff), &my_charset_bin);
    if ((res= ooa_arg->val_str(&tmp)) == NULL)
      return TRUE;

    *mode_one=eq_ascii_string(res->charset(), "one",
                             res->ptr(), res->length());
    if (!*mode_one)
    {
      if (!eq_ascii_string(res->charset(), "all", res->ptr(), res->length()))
      {
        THD *thd= current_thd;
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_JSON_ONE_OR_ALL, ER_THD(thd, ER_JSON_ONE_OR_ALL),
                            f->func_name());
        *mode_one= TRUE;
        return TRUE;
      }
    }
    *ooa_parsed= ooa_constant;
  }
  return FALSE;
}


#ifdef DUMMY
longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto return_null;

  result= !mode_one;
  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    int array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_arg - 2;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-2));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto null_return;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }

    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;
    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto js_error;

      if (!mode_one)
      {
        result= 0;
        break;
      }
    }
    else if (mode_one)
    {
      result= 1;
      break;
    }
  }


  return result;

js_error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}
#endif /*DUMMY*/

longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;
  json_path_t p;
  int n_found;
  LINT_INIT(n_found);
  int array_sizes[JSON_DEPTH_LIMIT];
  uint has_negative_path= 0;

  if ((null_value= args[0]->null_value))
    return 0;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto null_return;;

  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 2;
    c_path->p.types_used= JSON_PATH_KEY_NULL;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-2));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto null_return;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }
    if (args[n_arg]->null_value)
      goto null_return;
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);


  if (!mode_one)
  {
    bzero(p_found, (arg_count-2) * sizeof(bool));
    n_found= arg_count - 2;
  }
  else
    n_found= 0; /* Just to prevent 'uninitialized value' warnings */

  result= 0;
  while (json_get_path_next(&je, &p) == 0)
  {
    int n_path= arg_count - 2;
    if (has_negative_path && je.value_type == JSON_VALUE_ARRAY &&
        json_skip_array_and_count(&je, array_sizes + (p.last_step - p.steps)))
    {
      result= 1;
      break;
    }

    json_path_with_flags *c_path= paths;
    for (; n_path > 0; n_path--, c_path++)
    {
      if (json_path_compare(&c_path->p, &p, je.value_type, array_sizes) >= 0)
      {
        if (mode_one)
        {
          result= 1;
          break;
        }
        /* mode_all */
        if (p_found[n_path-1])
          continue; /* already found */
        if (--n_found == 0)
        {
          result= 1;
          break;
        }
        p_found[n_path-1]= TRUE;
      }
    }
  }

  if (likely(je.s.error == 0))
    return result;

  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


/*
  This reproduces behavior according to the former
  Item_func_conv_charset::is_json_type() which returned args[0]->is_json_type().
  JSON functions with multiple string input with different character sets
  wrap some arguments into Item_func_conv_charset. So the former
  Item_func_conv_charset::is_json_type() took the JSON propery from args[0],
  i.e. from the original argument before the conversion.
  This is probably not always correct because an *explicit*
  `CONVERT(arg USING charset)` is actually a general purpose string
  expression, not a JSON expression.
*/
bool is_json_type(const Item *item)
{
  for ( ; ; )
  {
    if (Type_handler_json_common::is_json_type_handler(item->type_handler()))
      return true;
    const Item_func_conv_charset *func;
    if (!(func= dynamic_cast<const Item_func_conv_charset*>(item)))
      return false;
    item= func->arguments()[0];
  }
  return false;
}


static int append_json_value(String *str, Item *item, String *tmp_val)
{
  if (item->type_handler()->is_bool_type())
  {
    longlong v_int= item->val_int();
    const char *t_f;
    int t_f_len;

    if (item->null_value)
      goto append_null;

    if (v_int)
    {
      t_f= "true";
      t_f_len= 4;
    }
    else
    {
      t_f= "false";
      t_f_len= 5;
    }

    return str->append(t_f, t_f_len);
  }
  {
    String *sv= item->val_json(tmp_val);
    if (item->null_value)
      goto append_null;
    if (is_json_type(item))
      return str->append(sv->ptr(), sv->length());

    if (item->result_type() == STRING_RESULT)
    {
      return str->append('"') ||
             st_append_escaped(str, sv) ||
             str->append('"');
    }
    return st_append_escaped(str, sv);
  }

append_null:
  return str->append(STRING_WITH_LEN("null"));
}


static int append_json_value_from_field(String *str,
  Item *i, Field *f, const uchar *key, size_t offset, String *tmp_val)
{
  if (i->type_handler()->is_bool_type())
  {
    longlong v_int= f->val_int(key + offset);
    const char *t_f;
    int t_f_len;

    if (f->is_null_in_record(key))
      goto append_null;

    if (v_int)
    {
      t_f= "true";
      t_f_len= 4;
    }
    else
    {
      t_f= "false";
      t_f_len= 5;
    }

    return str->append(t_f, t_f_len);
  }
  {
    String *sv= f->val_str(tmp_val, key + offset);
    if (f->is_null_in_record(key))
      goto append_null;
    if (is_json_type(i))
      return str->append(sv->ptr(), sv->length());

    if (i->result_type() == STRING_RESULT)
    {
      return str->append('"') ||
             st_append_escaped(str, sv) ||
             str->append('"');
    }
    return st_append_escaped(str, sv);
  }

append_null:
  return str->append(STRING_WITH_LEN("null"));
}


static int append_json_keyname(String *str, Item *item, String *tmp_val)
{
  String *sv= item->val_str(tmp_val);
  if (item->null_value)
    goto append_null;

  return str->append('"') ||
         st_append_escaped(str, sv) ||
         str->append("\": ", 3);

append_null:
  return str->append("\"\": ", 4);
}


bool Item_func_json_array::fix_length_and_dec(THD *thd)
{
  ulonglong char_length= 2;
  uint n_arg;

  result_limit= 0;

  if (arg_count == 0)
  {
    THD* thd= current_thd;
    collation.set(thd->variables.collation_connection,
                  DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
    tmp_val.set_charset(thd->variables.collation_connection);
    max_length= 2;
    return FALSE;
  }

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return TRUE;

  for (n_arg=0 ; n_arg < arg_count ; n_arg++)
    char_length+= args[n_arg]->max_char_length() + 4;

  fix_char_length_ulonglong(char_length);
  tmp_val.set_charset(collation.collation);
  return FALSE;
}


String *Item_func_json_array::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint n_arg;

  str->length(0);
  str->set_charset(collation.collation);

  if (str->append('[') ||
      ((arg_count > 0) && append_json_value(str, args[0], &tmp_val)))
    goto err_return;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    if (str->append(", ", 2) ||
        append_json_value(str, args[n_arg], &tmp_val))
      goto err_return;
  }

  if (str->append(']'))
    goto err_return;

  if (result_limit == 0)
    result_limit= current_thd->variables.max_allowed_packet;

  if (str->length() <= result_limit)
    return str;

  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
      func_name(), result_limit);

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


bool Item_func_json_array_append::fix_length_and_dec(THD *thd)
{
  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  char_length= args[0]->max_char_length();

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg/2].set_constant_flag(args[n_arg]->const_item());
    char_length+= args[n_arg/2+1]->max_char_length() + 4;
  }

  fix_char_length_ulonglong(char_length);
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_array_append::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  size_t str_rest_len;
  const uchar *ar_end;
  THD *thd= current_thd;

  DBUG_ASSERT(fixed());

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p &&
          path_setup_nwc(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &c_path->p, n_arg);
        goto return_null;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uchar*)&thd->killed;

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;

      goto return_null;
    }

    if (json_read_value(&je))
      goto js_error;

    str->length(0);
    str->set_charset(js->charset());
    if (str->reserve(js->length() + 8, 1024))
      goto return_null; /* Out of memory. */

    if (je.value_type == JSON_VALUE_ARRAY)
    {
      int n_items;
      if (json_skip_level_and_count(&je, &n_items))
        goto js_error;

      ar_end= je.s.c_str - je.sav_c_len;
      str_rest_len= js->length() - (ar_end - (const uchar *) js->ptr());
      str->q_append(js->ptr(), ar_end-(const uchar *) js->ptr());
      if (n_items)
        str->append(", ", 2);
      if (append_json_value(str, args[n_arg+1], &tmp_val))
        goto return_null; /* Out of memory. */

      if (str->reserve(str_rest_len, 1024))
        goto return_null; /* Out of memory. */
      str->q_append((const char *) ar_end, str_rest_len);
    }
    else
    {
      const uchar *c_from, *c_to;

      /* Wrap as an array. */
      str->q_append(js->ptr(), (const char *) je.value_begin - js->ptr());
      c_from= je.value_begin;

      if (je.value_type == JSON_VALUE_OBJECT)
      {
        if (json_skip_level(&je))
          goto js_error;
        c_to= je.s.c_str;
      }
      else
        c_to= je.value_end;

      if (str->append('[') ||
          str->append((const char *) c_from, c_to - c_from) ||
          str->append(", ", 2) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          str->append(']') ||
          str->append((const char *) je.s.c_str,
                      js->end() - (const char *) je.s.c_str))
        goto return_null; /* Out of memory. */
    }
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uchar*)&thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  return str;

js_error:
  report_json_error(js, &je, 0);

return_null:
  thd->check_killed(); // to get the error message right
  null_value= 1;
  return 0;
}


String *Item_func_json_array_insert::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  THD *thd= current_thd;

  DBUG_ASSERT(fixed());

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    const char *item_pos;
    int n_item, corrected_n_item;

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p &&
          (path_setup_nwc(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()) ||
           c_path->p.last_step - 1 < c_path->p.steps ||
           c_path->p.last_step->type != JSON_PATH_ARRAY))
      {
        if (c_path->p.s.error == 0)
          c_path->p.s.error= SHOULD_END_WITH_ARRAY;
        
        report_path_error(s_p, &c_path->p, n_arg);

        goto return_null;
      }
      c_path->parsed= c_path->constant;
      c_path->p.last_step--;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uchar*)&thd->killed;

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;

      /* Can't find the array to insert. */
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    if (je.value_type != JSON_VALUE_ARRAY)
    {
      /* Must be an array. */
      continue;
    }

    item_pos= 0;
    n_item= 0;
    corrected_n_item= c_path->p.last_step[1].n_item;
    if (corrected_n_item < 0)
    {
      int array_size;
      if (json_skip_array_and_count(&je, &array_size))
        goto js_error;
      corrected_n_item+= array_size + 1;
    }

    while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
    {
      DBUG_ASSERT(je.state == JST_VALUE);

      if (n_item == corrected_n_item)
      {
        item_pos= (const char *) je.s.c_str;
        break;
      }
      n_item++;

      if (json_read_value(&je) ||
          (!json_value_scalar(&je) && json_skip_level(&je)))
        goto js_error;
    }

    if (unlikely(je.s.error || *je.killed_ptr))
      goto js_error;

    str->length(0);
    str->set_charset(js->charset());
    if (item_pos)
    {
      if (append_simple(str, js->ptr(), item_pos - js->ptr()) ||
          (n_item > 0  && str->append(" ", 1)) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          str->append(",", 1) ||
          (n_item == 0  && str->append(" ", 1)) ||
          append_simple(str, item_pos, js->end() - item_pos))
        goto return_null; /* Out of memory. */
    }
    else
    {
      /* Insert position wasn't found - append to the array. */
      DBUG_ASSERT(je.state == JST_ARRAY_END);
      item_pos= (const char *) (je.s.c_str - je.sav_c_len);
      if (append_simple(str, js->ptr(), item_pos - js->ptr()) ||
          (n_item > 0  && str->append(", ", 2)) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          append_simple(str, item_pos, js->end() - item_pos))
        goto return_null; /* Out of memory. */
    }

    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uchar*)&thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  return str;

js_error:
  report_json_error(js, &je, 0);
return_null:
  thd->check_killed(); // to get the error message right
  null_value= 1;
  return 0;
}


String *Item_func_json_object::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint n_arg;

  str->length(0);
  str->set_charset(collation.collation);

  if (str->append('{') ||
      (arg_count > 0 &&
       (append_json_keyname(str, args[0], &tmp_val) ||
        append_json_value(str, args[1], &tmp_val))))
    goto err_return;

  for (n_arg=2; n_arg < arg_count; n_arg+=2)
  {
    if (str->append(", ", 2) ||
        append_json_keyname(str, args[n_arg], &tmp_val) ||
        append_json_value(str, args[n_arg+1], &tmp_val))
      goto err_return;
  }

  if (str->append('}'))
    goto err_return;

  if (result_limit == 0)
    result_limit= current_thd->variables.max_allowed_packet;

  if (str->length() <= result_limit)
    return str;

  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
      func_name(), result_limit);

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


static int do_merge(String *str, json_engine_t *je1, json_engine_t *je2)
{

  long arbitrary_var;
  long stack_used_up= (available_stack_size(current_thd->thread_stack, &arbitrary_var));
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  {ALLOCATE_MEM_ON_STACK(my_thread_stack_size-stack_used_up-STACK_MIN_SIZE);});
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  if (json_read_value(je1) || json_read_value(je2))
    return 1;

  if (je1->value_type == JSON_VALUE_OBJECT &&
      je2->value_type == JSON_VALUE_OBJECT)
  {
    json_engine_t sav_je1= *je1;
    json_engine_t sav_je2= *je2;

    int first_key= 1;
    json_string_t key_name;
  
    json_string_set_cs(&key_name, je1->s.cs);

    if (str->append('{'))
      return 3;
    while (json_scan_next(je1) == 0 &&
           je1->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      /* Loop through the Json_1 keys and compare with the Json_2 keys. */
      DBUG_ASSERT(je1->state == JST_KEY);
      key_start= je1->s.c_str;
      do
      {
        key_end= je1->s.c_str;
      } while (json_read_keyname_chr(je1) == 0);

      if (unlikely(je1->s.error))
        return 1;

      if (first_key)
        first_key= 0;
      else
      {
        if (str->append(", ", 2))
          return 3;
        *je2= sav_je2;
      }

      if (str->append('"') ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append("\":", 2))
        return 3;

      while (json_scan_next(je2) == 0 &&
          je2->state != JST_OBJ_END)
      {
        int ires;
        DBUG_ASSERT(je2->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je2, &key_name))
        {
          if (je2->s.error || json_skip_key(je2))
            return 2;
          continue;
        }

        /* Json_2 has same key as Json_1. Merge them. */
        if ((ires= do_merge(str, je1, je2)))
          return ires;
        goto merged_j1;
      }
      if (unlikely(je2->s.error))
        return 2;

      key_start= je1->s.c_str;
      /* Just append the Json_1 key value. */
      if (json_skip_key(je1))
        return 1;
      if (append_simple(str, key_start, je1->s.c_str - key_start))
        return 3;

merged_j1:
      continue;
    }

    *je2= sav_je2;
    /*
      Now loop through the Json_2 keys.
      Skip if there is same key in Json_1
    */
    while (json_scan_next(je2) == 0 &&
           je2->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      DBUG_ASSERT(je2->state == JST_KEY);
      key_start= je2->s.c_str;
      do
      {
        key_end= je2->s.c_str;
      } while (json_read_keyname_chr(je2) == 0);

      if (unlikely(je2->s.error))
        return 1;

      *je1= sav_je1;
      while (json_scan_next(je1) == 0 &&
             je1->state != JST_OBJ_END)
      {
        DBUG_ASSERT(je1->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je1, &key_name))
        {
          if (unlikely(je1->s.error || json_skip_key(je1)))
            return 2;
          continue;
        }
        if (json_skip_key(je2) || json_skip_level(je1))
          return 1;
        goto continue_j2;
      }

      if (unlikely(je1->s.error))
        return 2;

      if (first_key)
        first_key= 0;
      else if (str->append(", ", 2))
        return 3;

      if (json_skip_key(je2))
        return 1;

      if (str->append('"') ||
          append_simple(str, key_start, je2->s.c_str - key_start))
        return 3;

continue_j2:
      continue;
    }

    if (str->append('}'))
      return 3;
  }
  else
  {
    const uchar *end1, *beg1, *end2, *beg2;
    int n_items1=1, n_items2= 1;

    beg1= je1->value_begin;

    /* Merge as a single array. */
    if (je1->value_type == JSON_VALUE_ARRAY)
    {
      if (json_skip_level_and_count(je1, &n_items1))
        return 1;

      end1= je1->s.c_str - je1->sav_c_len;
    }
    else
    {
      if (str->append('['))
        return 3;
      if (je1->value_type == JSON_VALUE_OBJECT)
      {
        if (json_skip_level(je1))
          return 1;
        end1= je1->s.c_str;
      }
      else
        end1= je1->value_end;
    }

    if (str->append((const char*) beg1, end1 - beg1))
      return 3;

    if (json_value_scalar(je2))
    {
      beg2= je2->value_begin;
      end2= je2->value_end;
    }
    else
    {
      if (je2->value_type == JSON_VALUE_OBJECT)
      {
        beg2= je2->value_begin;
        if (json_skip_level(je2))
          return 2;
      }
      else
      {
        beg2= je2->s.c_str;
        if (json_skip_level_and_count(je2, &n_items2))
          return 2;
      }
      end2= je2->s.c_str;
    }

    if ((n_items1 && n_items2 && str->append(", ", 2)) ||
        str->append((const char*) beg2, end2 - beg2))
      return 3;

    if (je2->value_type != JSON_VALUE_ARRAY &&
        str->append(']'))
      return 3;
  }

  return 0;
}


String *Item_func_json_merge::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  json_engine_t je1, je2;
  String *js1= args[0]->val_json(&tmp_js1), *js2=NULL;
  uint n_arg;
  THD *thd= current_thd;
  LINT_INIT(js2);

  if (args[0]->null_value)
    goto null_return;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    str->set_charset(js1->charset());
    str->length(0);

    js2= args[n_arg]->val_json(&tmp_js2);
    if (args[n_arg]->null_value)
      goto null_return;

    json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                    (const uchar *) js1->ptr() + js1->length());
    je1.killed_ptr= (uchar*)&thd->killed;

    json_scan_start(&je2, js2->charset(),(const uchar *) js2->ptr(),
                    (const uchar *) js2->ptr() + js2->length());
    je2.killed_ptr= (uchar*)&thd->killed;

    if (do_merge(str, &je1, &je2))
      goto error_return;

    {
      /* Swap str and js1. */
      if (str == &tmp_js1)
      {
        str= js1;
        js1= &tmp_js1;
      }
      else
      {
        js1= str;
        str= &tmp_js1;
      }
    }
  }

  json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                  (const uchar *) js1->ptr() + js1->length());
  je1.killed_ptr= (uchar*)&thd->killed;
  if (json_nice(&je1, str, Item_func_json_format::LOOSE))
    goto error_return;

  null_value= 0;
  return str;

error_return:
  if (je1.s.error)
    report_json_error(js1, &je1, 0);
  if (je2.s.error)
    report_json_error(js2, &je2, n_arg);
  thd->check_killed(); // to get the error message right
null_return:
  null_value= 1;
  return NULL;
}


static int copy_value_patch(String *str, json_engine_t *je)
{
  int first_key= 1;

  if (je->value_type != JSON_VALUE_OBJECT)
  {
    const uchar *beg, *end;

    beg= je->value_begin;

    if (!json_value_scalar(je))
    {
      if (json_skip_level(je))
        return 1;
      end= je->s.c_str;
    }
    else
      end= je->value_end;

    if (append_simple(str, beg, end-beg))
      return 1;

    return 0;
  }
  /* JSON_VALUE_OBJECT */

  if (str->append('{'))
    return 1;
  while (json_scan_next(je) == 0 && je->state != JST_OBJ_END)
  {
    const uchar *key_start;
    /* Loop through the Json_1 keys and compare with the Json_2 keys. */
    DBUG_ASSERT(je->state == JST_KEY);
    key_start= je->s.c_str;

    if (json_read_value(je))
      return 1;

    if (je->value_type == JSON_VALUE_NULL)
      continue;

    if (!first_key)
    {
      if (str->append(", ", 2))
        return 3;
    }
    else
      first_key= 0;

    if (str->append('"') ||
        append_simple(str, key_start, je->value_begin - key_start) ||
        copy_value_patch(str, je))
      return 1;
  }
  if (str->append('}'))
    return 1;

  return 0;
}


static int do_merge_patch(String *str, json_engine_t *je1, json_engine_t *je2,
                          bool *empty_result)
{
  long arbitrary_var;
  long stack_used_up= (available_stack_size(current_thd->thread_stack, &arbitrary_var));
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  {ALLOCATE_MEM_ON_STACK(my_thread_stack_size-stack_used_up-STACK_MIN_SIZE);});
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  if (json_read_value(je1) || json_read_value(je2))
    return 1;

  if (je1->value_type == JSON_VALUE_OBJECT &&
      je2->value_type == JSON_VALUE_OBJECT)
  {
    json_engine_t sav_je1= *je1;
    json_engine_t sav_je2= *je2;

    int first_key= 1;
    json_string_t key_name;
    size_t sav_len;
    bool mrg_empty;

    *empty_result= FALSE;
    json_string_set_cs(&key_name, je1->s.cs);

    if (str->append('{'))
      return 3;
    while (json_scan_next(je1) == 0 &&
           je1->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      /* Loop through the Json_1 keys and compare with the Json_2 keys. */
      DBUG_ASSERT(je1->state == JST_KEY);
      key_start= je1->s.c_str;
      do
      {
        key_end= je1->s.c_str;
      } while (json_read_keyname_chr(je1) == 0);

      if (je1->s.error)
        return 1;

      sav_len= str->length();

      if (!first_key)
      {
        if (str->append(", ", 2))
          return 3;
        *je2= sav_je2;
      }

      if (str->append('"') ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append("\":", 2))
        return 3;

      while (json_scan_next(je2) == 0 &&
          je2->state != JST_OBJ_END)
      {
        int ires;
        DBUG_ASSERT(je2->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je2, &key_name))
        {
          if (je2->s.error || json_skip_key(je2))
            return 2;
          continue;
        }

        /* Json_2 has same key as Json_1. Merge them. */
        if ((ires= do_merge_patch(str, je1, je2, &mrg_empty)))
          return ires;

        if (mrg_empty)
          str->length(sav_len);
        else
          first_key= 0;

        goto merged_j1;
      }

      if (je2->s.error)
        return 2;

      key_start= je1->s.c_str;
      /* Just append the Json_1 key value. */
      if (json_skip_key(je1))
        return 1;
      if (append_simple(str, key_start, je1->s.c_str - key_start))
        return 3;
      first_key= 0;

merged_j1:
      continue;
    }

    *je2= sav_je2;
    /*
      Now loop through the Json_2 keys.
      Skip if there is same key in Json_1
    */
    while (json_scan_next(je2) == 0 &&
           je2->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      DBUG_ASSERT(je2->state == JST_KEY);
      key_start= je2->s.c_str;
      do
      {
        key_end= je2->s.c_str;
      } while (json_read_keyname_chr(je2) == 0);

      if (je2->s.error)
        return 1;

      *je1= sav_je1;
      while (json_scan_next(je1) == 0 &&
             je1->state != JST_OBJ_END)
      {
        DBUG_ASSERT(je1->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je1, &key_name))
        {
          if (je1->s.error || json_skip_key(je1))
            return 2;
          continue;
        }
        if (json_skip_key(je2) ||
            json_skip_level(je1))
          return 1;
        goto continue_j2;
      }

      if (je1->s.error)
        return 2;


      sav_len= str->length();

      if (!first_key && str->append(", ", 2))
        return 3;

      if (str->append('"') ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append("\":", 2))
        return 3;

      if (json_read_value(je2))
        return 1;

      if (je2->value_type == JSON_VALUE_NULL)
        str->length(sav_len);
      else
      {
        if (copy_value_patch(str, je2))
          return 1;
        first_key= 0;
      }

continue_j2:
      continue;
    }

    if (str->append('}'))
      return 3;
  }
  else
  {
    if (!json_value_scalar(je1) && json_skip_level(je1))
      return 1;

    *empty_result= je2->value_type == JSON_VALUE_NULL;
    if (!(*empty_result) && copy_value_patch(str, je2))
      return 1;
  }

  return 0;
}


String *Item_func_json_merge_patch::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  json_engine_t je1, je2;
  String *js1= args[0]->val_json(&tmp_js1), *js2=NULL;
  uint n_arg;
  bool empty_result, merge_to_null;
  THD *thd= current_thd;

  /* To report errors properly if some JSON is invalid. */
  je1.s.error= je2.s.error= 0;
  merge_to_null= args[0]->null_value;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    js2= args[n_arg]->val_json(&tmp_js2);
    if (args[n_arg]->null_value)
    {
      merge_to_null= true;
      goto cont_point;
    }

    json_scan_start(&je2, js2->charset(),(const uchar *) js2->ptr(),
                    (const uchar *) js2->ptr() + js2->length());
    je2.killed_ptr= (uchar*)&thd->killed;

    if (merge_to_null)
    {
      if (json_read_value(&je2))
        goto error_return;
      if (je2.value_type == JSON_VALUE_OBJECT)
      {
        merge_to_null= true;
        goto cont_point;
      }
      merge_to_null= false;
      str->set(js2->ptr(), js2->length(), js2->charset());
      goto cont_point;
    }

    str->set_charset(js1->charset());
    str->length(0);


    json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                    (const uchar *) js1->ptr() + js1->length());
    je1.killed_ptr= (uchar*)&thd->killed;

    if (do_merge_patch(str, &je1, &je2, &empty_result))
      goto error_return;

    if (empty_result)
      str->append(STRING_WITH_LEN("null"));

cont_point:
    {
      /* Swap str and js1. */
      if (str == &tmp_js1)
      {
        str= js1;
        js1= &tmp_js1;
      }
      else
      {
        js1= str;
        str= &tmp_js1;
      }
    }
  }

  if (merge_to_null)
    goto null_return;

  json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                  (const uchar *) js1->ptr() + js1->length());
  je1.killed_ptr= (uchar*)&thd->killed;
  if (json_nice(&je1, str, Item_func_json_format::LOOSE))
    goto error_return;

  null_value= 0;
  return str;

error_return:
  if (je1.s.error)
    report_json_error(js1, &je1, 0);
  if (je2.s.error)
    report_json_error(js2, &je2, n_arg);
  thd->check_killed(); // to get the error message right
null_return:
  null_value= 1;
  return NULL;
}


bool Item_func_json_length::fix_length_and_dec(THD *thd)
{
  if (arg_count > 1)
    path.set_constant_flag(args[1]->const_item());
  set_maybe_null();
  max_length= 10;
  return FALSE;
}


longlong Item_func_json_length::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint length= 0;
  int array_counters[JSON_DEPTH_LIMIT];
  int err;

  if ((null_value= args[0]->null_value))
    return 0;

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count > 1)
  {
    /* Path specified - let's apply it. */
    if (!path.parsed)
    {
      String *s_p= args[1]->val_str(&tmp_path);
      if (s_p &&
          path_setup_nwc(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &path.p, 1);
        goto null_return;
      }
      path.parsed= path.constant;
    }
    if (args[1]->null_value)
      goto null_return;

    path.cur_step= path.p.steps;
    if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
    {
      if (je.s.error)
        goto err_return;
      goto null_return;
    }
  }
  

  if (json_read_value(&je))
    goto err_return;

  if (json_value_scalar(&je))
    return 1;

  while (!(err= json_scan_next(&je)) &&
         je.state != JST_OBJ_END && je.state != JST_ARRAY_END)
  {
    switch (je.state)
    {
    case JST_VALUE:
    case JST_KEY:
      length++;
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      if (json_skip_level(&je))
        goto err_return;
      break;
    default:
      break;
    };
  }

  if (!err)
  {
    /* Parse to the end of the JSON just to check it's valid. */
    while (json_scan_next(&je) == 0) {}
  }

  if (likely(!je.s.error))
    return length;

err_return:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


longlong Item_func_json_depth::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint depth= 0, c_depth= 0;
  bool inc_depth= TRUE;

  if ((null_value= args[0]->null_value))
    return 0;


  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  do
  {
    switch (je.state)
    {
    case JST_VALUE:
    case JST_KEY:
      if (inc_depth)
      {
        c_depth++;
        inc_depth= FALSE;
        if (c_depth > depth)
          depth= c_depth;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      inc_depth= TRUE;
      break;
    case JST_OBJ_END:
    case JST_ARRAY_END:
      if (!inc_depth)
        c_depth--;
      inc_depth= FALSE;
      break;
    default:
      break;
    }
  } while (json_scan_next(&je) == 0);

  if (likely(!je.s.error))
    return depth;

  report_json_error(js, &je, 0);
  null_value= 1;
  return 0;
}


bool Item_func_json_type::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb3_general_ci);
  max_length= 12 * collation.collation->mbmaxlen;
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_type::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  const char *type;

  if ((null_value= args[0]->null_value))
    return 0;


  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (json_read_value(&je))
    goto error;

  switch (je.value_type)
  {
  case JSON_VALUE_OBJECT:
    type= "OBJECT";
    break;
  case JSON_VALUE_ARRAY:
    type= "ARRAY";
    break;
  case JSON_VALUE_STRING:
    type= "STRING";
    break;
  case JSON_VALUE_NUMBER:
    type= (je.num_flags & JSON_NUM_FRAC_PART) ?  "DOUBLE" : "INTEGER";
    break;
  case JSON_VALUE_TRUE:
  case JSON_VALUE_FALSE:
    type= "BOOLEAN";
    break;
  default:
    type= "NULL";
    break;
  }

  str->set(type, strlen(type), &my_charset_utf8mb3_general_ci);
  return str;

error:
  report_json_error(js, &je, 0);
  null_value= 1;
  return 0;
}


bool Item_func_json_insert::fix_length_and_dec(THD *thd)
{
  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  char_length= args[0]->max_char_length();

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg/2].set_constant_flag(args[n_arg]->const_item());
    char_length+= args[n_arg/2+1]->max_char_length() + 4;
  }

  fix_char_length_ulonglong(char_length);
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_insert::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  json_string_t key_name;
  THD *thd= current_thd;

  DBUG_ASSERT(fixed());

  if ((null_value= args[0]->null_value))
    return 0;

  str->set_charset(collation.collation);
  tmp_js.set_charset(collation.collation);
  json_string_set_cs(&key_name, collation.collation);

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    const char *v_to;
    json_path_step_t *lp;
    int corrected_n_item;

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p)
      {
        if (path_setup_nwc(&c_path->p,s_p->charset(),
                           (const uchar *) s_p->ptr(),
                           (const uchar *) s_p->ptr() + s_p->length()))
        {
          report_path_error(s_p, &c_path->p, n_arg);
          goto return_null;
        }

        /* We search to the last step. */
        c_path->p.last_step--;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uchar*)&thd->killed;

    if (c_path->p.last_step < c_path->p.steps)
      goto v_found;

    c_path->cur_step= c_path->p.steps;

    if (c_path->p.last_step >= c_path->p.steps &&
        json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    lp= c_path->p.last_step+1;
    if (lp->type & JSON_PATH_ARRAY)
    {
      int n_item= 0;

      if (je.value_type != JSON_VALUE_ARRAY)
      {
        const uchar *v_from= je.value_begin;
        int do_array_autowrap;

        if (mode_insert)
        {
          if (mode_replace)
            do_array_autowrap= lp->n_item > 0;
          else
          {
            if (lp->n_item == 0)
              continue;
            do_array_autowrap= 1;
          }
        }
        else
        {
          if (lp->n_item)
            continue;
          do_array_autowrap= 0;
        }


        str->length(0);
        /* Wrap the value as an array. */
        if (append_simple(str, js->ptr(), (const char *) v_from - js->ptr()) ||
            (do_array_autowrap && str->append('[')))
          goto js_error; /* Out of memory. */

        if (je.value_type == JSON_VALUE_OBJECT)
        {
          if (json_skip_level(&je))
            goto js_error;
        }

        if ((do_array_autowrap &&
             (append_simple(str, v_from, je.s.c_str - v_from) ||
              str->append(", ", 2))) ||
            append_json_value(str, args[n_arg+1], &tmp_val) ||
            (do_array_autowrap && str->append(']')) ||
            append_simple(str, je.s.c_str, js->end()-(const char *) je.s.c_str))
          goto js_error; /* Out of memory. */

        goto continue_point;
      }
      corrected_n_item= lp->n_item;
      if (corrected_n_item < 0)
      {
        int array_size;
        if (json_skip_array_and_count(&je, &array_size))
          goto js_error;
        corrected_n_item+= array_size;
      }

      while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
      {
        switch (je.state)
        {
        case JST_VALUE:
          if (n_item == corrected_n_item)
            goto v_found;
          n_item++;
          if (json_skip_array_item(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      if (!mode_insert)
        continue;

      v_to= (const char *) (je.s.c_str - je.sav_c_len);
      str->length(0);
      if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
          (n_item > 0 && str->append(", ", 2)) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          append_simple(str, v_to, js->end() - v_to))
        goto js_error; /* Out of memory. */
    }
    else /*JSON_PATH_KEY*/
    {
      uint n_key= 0;

      if (je.value_type != JSON_VALUE_OBJECT)
        continue;

      while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
      {
        switch (je.state)
        {
        case JST_KEY:
          json_string_set_str(&key_name, lp->key, lp->key_end);
          if (json_key_matches(&je, &key_name))
            goto v_found;
          n_key++;
          if (json_skip_key(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      if (!mode_insert)
        continue;

      v_to= (const char *) (je.s.c_str - je.sav_c_len);
      str->length(0);
      if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
          (n_key > 0 && str->append(", ", 2)) ||
          str->append('"') ||
          append_simple(str, lp->key, lp->key_end - lp->key) ||
          str->append("\":", 2) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          append_simple(str, v_to, js->end() - v_to))
        goto js_error; /* Out of memory. */
    }

    goto continue_point;

v_found:

    if (!mode_replace)
      continue;

    if (json_read_value(&je))
      goto js_error;

    v_to= (const char *) je.value_begin;
    str->length(0);
    if (!json_value_scalar(&je))
    {
      if (json_skip_level(&je))
        goto js_error;
    }

    if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
        append_json_value(str, args[n_arg+1], &tmp_val) ||
        append_simple(str, je.s.c_str, js->end()-(const char *) je.s.c_str))
      goto js_error; /* Out of memory. */
continue_point:
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uchar*)&thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  return str;

js_error:
  report_json_error(js, &je, 0);
  thd->check_killed(); // to get the error message right
return_null:
  null_value= 1;
  return 0;
}


bool Item_func_json_remove::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;

  mark_constant_paths(paths, args+1, arg_count-1);
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_remove::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  json_string_t key_name;
  THD *thd= current_thd;

  DBUG_ASSERT(fixed());

  if (args[0]->null_value)
    goto null_return;

  str->set_charset(js->charset());
  json_string_set_cs(&key_name, js->charset());

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg++, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    const char *rem_start= 0, *rem_end;
    json_path_step_t *lp;
    int n_item= 0;

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p)
      {
        if (path_setup_nwc(&c_path->p,s_p->charset(),
                           (const uchar *) s_p->ptr(),
                           (const uchar *) s_p->ptr() + s_p->length()))
        {
          report_path_error(s_p, &c_path->p, n_arg);
          goto null_return;
        }

        /* We search to the last step. */
        c_path->p.last_step--;
        if (c_path->p.last_step < c_path->p.steps)
        {
          c_path->p.s.error= TRIVIAL_PATH_NOT_ALLOWED;
          report_path_error(s_p, &c_path->p, n_arg);
          goto null_return;
        }
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto null_return;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uchar*)&thd->killed;

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;
    }

    if (json_read_value(&je))
      goto js_error;

    lp= c_path->p.last_step+1;

    if (lp->type & JSON_PATH_ARRAY)
    {
      int corrected_n_item;
      if (je.value_type != JSON_VALUE_ARRAY)
        continue;

      corrected_n_item= lp->n_item;
      if (corrected_n_item < 0)
      {
        int array_size;
        if (json_skip_array_and_count(&je, &array_size))
          goto js_error;
        corrected_n_item+= array_size;
      }

      while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
      {
        switch (je.state)
        {
        case JST_VALUE:
          if (n_item == corrected_n_item)
          {
            rem_start= (const char *) (je.s.c_str -
                         (n_item ? je.sav_c_len : 0));
            goto v_found;
          }
          n_item++;
          if (json_skip_array_item(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      continue;
    }
    else /*JSON_PATH_KEY*/
    {
      if (je.value_type != JSON_VALUE_OBJECT)
        continue;

      while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
      {
        switch (je.state)
        {
        case JST_KEY:
          if (n_item == 0)
            rem_start= (const char *) (je.s.c_str - je.sav_c_len);
          json_string_set_str(&key_name, lp->key, lp->key_end);
          if (json_key_matches(&je, &key_name))
            goto v_found;

          if (json_skip_key(&je))
            goto js_error;

          rem_start= (const char *) je.s.c_str;
          n_item++;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      continue;
    }

v_found:

    if (json_skip_key(&je) || json_scan_next(&je))
      goto js_error;

    rem_end= (je.state == JST_VALUE && n_item == 0) ? 
      (const char *) je.s.c_str : (const char *) (je.s.c_str - je.sav_c_len);

    str->length(0);

    if (append_simple(str, js->ptr(), rem_start - js->ptr()) ||
        (je.state == JST_KEY && n_item > 0 && str->append(",", 1)) ||
        append_simple(str, rem_end, js->end() - rem_end))
          goto js_error; /* Out of memory. */

    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uchar*)&thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  null_value= 0;
  return str;

js_error:
  thd->check_killed(); // to get the error message right
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_keys::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_maybe_null();
  if (arg_count > 1)
    path.set_constant_flag(args[1]->const_item());
  return FALSE;
}


/*
  That function is for Item_func_json_keys::val_str exclusively.
  It utilizes the fact the resulting string is in specific format:
        ["key1", "key2"...]
*/
static int check_key_in_list(String *res,
                             const uchar *key, int key_len)
{
  const uchar *c= (const uchar *) res->ptr() + 2; /* beginning '["' */
  const uchar *end= (const uchar *) res->end() - 1; /* ending '"' */

  while (c < end)
  {
    int n_char;
    for (n_char=0; c[n_char] != '"' && n_char < key_len; n_char++)
    {
      if (c[n_char] != key[n_char])
        break;
    }
    if (c[n_char] == '"')
    {
      if (n_char == key_len)
        return 1;
    }
    else
    {
      while (c[n_char] != '"')
        n_char++;
    }
    c+= n_char + 4; /* skip ', "' */
  }
  return 0;
}


String *Item_func_json_keys::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_keys= 0;
  int array_counters[JSON_DEPTH_LIMIT];

  if ((args[0]->null_value))
    goto null_return;

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count < 2)
    goto skip_search;

  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        path_setup_nwc(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                       (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &path.p, 1);
        goto null_return;
      }
    path.parsed= path.constant;
  }

  if (args[1]->null_value)
    goto null_return;

  path.cur_step= path.p.steps;

  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto err_return;

    goto null_return;
  }

skip_search:
  if (json_read_value(&je))
    goto err_return;

  if (je.value_type != JSON_VALUE_OBJECT)
    goto null_return;
  
  str->length(0);
  if (str->append('['))
    goto err_return; /* Out of memory. */
  /* Parse the OBJECT collecting the keys. */
  while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
  {
    const uchar *key_start, *key_end;
    int key_len;

    switch (je.state)
    {
    case JST_KEY:
      key_start= je.s.c_str;
      do
      {
        key_end= je.s.c_str;
      } while (json_read_keyname_chr(&je) == 0);
      if (unlikely(je.s.error))
        goto err_return;
      key_len= (int)(key_end - key_start);

      if (!check_key_in_list(str, key_start, key_len))
      { 
        if ((n_keys > 0 && str->append(", ", 2)) ||
          str->append('"') ||
          append_simple(str, key_start, key_len) ||
          str->append('"'))
        goto err_return;
        n_keys++;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      if (json_skip_level(&je))
        break;
      break;
    default:
      break;
    }
  }

  if (unlikely(je.s.error || str->append(']')))
    goto err_return;

  null_value= 0;
  return str;

err_return:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_search::fix_fields(THD *thd, Item **ref)
{
  if (Item_json_str_multipath::fix_fields(thd, ref))
    return TRUE;

  if (arg_count < 4)
  {
    escape= '\\';
    return FALSE;
  }

  return fix_escape_item(thd, args[3], &tmp_js, true,
                         args[0]->collation.collation, &escape);
}


static const uint SQR_MAX_BLOB_WIDTH= (uint) sqrt(MAX_BLOB_WIDTH);

bool Item_func_json_search::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);

  /*
    It's rather difficult to estimate the length of the result.
    I believe arglen^2 is the reasonable upper limit.
  */
  if (args[0]->max_length > SQR_MAX_BLOB_WIDTH)
    max_length= MAX_BLOB_WIDTH;
  else
  {
    max_length= args[0]->max_length;
    max_length*= max_length;
  }

  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;

  if (arg_count > 4)
    mark_constant_paths(paths, args+4, arg_count-4);
  set_maybe_null();
  return FALSE;
}


int Item_func_json_search::compare_json_value_wild(json_engine_t *je,
                                                   const String *cmp_str)
{
  if (je->value_type != JSON_VALUE_STRING || !je->value_escaped)
    return collation.collation->wildcmp(
        (const char *) je->value, (const char *) (je->value + je->value_len),
        cmp_str->ptr(), cmp_str->end(), escape, wild_one, wild_many) ? 0 : 1;

  {
    int esc_len;
    if (esc_value.alloced_length() < (uint) je->value_len &&
        esc_value.alloc((je->value_len / 1024 + 1) * 1024))
      return 0;

    esc_len= json_unescape(je->s.cs, je->value, je->value + je->value_len,
                           je->s.cs, (uchar *) esc_value.ptr(),
                           (uchar *) (esc_value.ptr() + 
                                      esc_value.alloced_length()));
    if (esc_len <= 0)
      return 0;

    return collation.collation->wildcmp(
        esc_value.ptr(), esc_value.ptr() + esc_len,
        cmp_str->ptr(), cmp_str->end(), escape, wild_one, wild_many) ? 0 : 1;
  }
}


static int append_json_path(String *str, const json_path_t *p)
{
  const json_path_step_t *c;

  if (str->append("\"$", 2))
    return TRUE;

  for (c= p->steps+1; c <= p->last_step; c++)
  {
    if (c->type & JSON_PATH_KEY)
    {
      if (str->append(".", 1) ||
          append_simple(str, c->key, c->key_end-c->key))
        return TRUE;
    }
    else /*JSON_PATH_ARRAY*/
    {

      if (str->append('[') ||
          str->append_ulonglong(c->n_item) ||
          str->append(']'))
        return TRUE;
    }
  }

  return str->append('"');
}


String *Item_func_json_search::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  String *s_str= args[2]->val_str(&tmp_path);
  json_engine_t je;
  json_path_t p, sav_path;
  uint n_arg;
  int array_sizes[JSON_DEPTH_LIMIT];
  uint has_negative_path= 0;

  if (args[0]->null_value || args[2]->null_value)
    goto null_return;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto null_return;

  n_path_found= 0;
  str->set_charset(js->charset());
  str->length(0);

  for (n_arg=4; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 4;
    c_path->p.types_used= JSON_PATH_KEY_NULL;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-4));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto null_return;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }
    if (args[n_arg]->null_value)
      goto null_return;
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {
    if (has_negative_path && je.value_type == JSON_VALUE_ARRAY &&
        json_skip_array_and_count(&je, array_sizes + (p.last_step - p.steps)))
      goto js_error;

    if (json_value_scalar(&je))
    {
      if ((arg_count < 5 ||
           path_ok(paths, arg_count - 4, &p, je.value_type, array_sizes)) &&
          compare_json_value_wild(&je, s_str) != 0)
      {
        ++n_path_found;
        if (n_path_found == 1)
        {
          sav_path= p;
          sav_path.last_step= sav_path.steps + (p.last_step - p.steps);
        }
        else
        {
          if (n_path_found == 2)
          {
            if (str->append('[') ||
                append_json_path(str, &sav_path))
                goto js_error;
          }
          if (str->append(", ", 2) || append_json_path(str, &p))
            goto js_error;
        }
        if (mode_one)
          goto end;
      }
    }
  }

  if (unlikely(je.s.error))
    goto js_error;

end:
  if (n_path_found == 0)
    goto null_return;
  if (n_path_found == 1)
  {
    if (append_json_path(str, &sav_path))
      goto js_error;
  }
  else
  {
    if (str->append(']'))
      goto js_error;
  }

  null_value= 0;
  return str;


js_error:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


LEX_CSTRING Item_func_json_format::func_name_cstring() const
{
  switch (fmt)
  {
  case COMPACT:
    return { STRING_WITH_LEN("json_compact") };
  case LOOSE:
    return { STRING_WITH_LEN("json_loose") };
  case DETAILED:
    return { STRING_WITH_LEN("json_detailed") };
  default:
    DBUG_ASSERT(0);
  };

  return NULL_clex_str;
}


bool Item_func_json_format::fix_length_and_dec(THD *thd)
{
  decimals= 0;
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_format::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  int tab_size= 4;
  THD *thd= current_thd;

  if ((null_value= args[0]->null_value))
    return 0;

  if (fmt == DETAILED)
  {
    if (arg_count > 1)
    {
      tab_size= (int)args[1]->val_int();
      if (args[1]->null_value)
      {
        null_value= 1;
        return 0;
      }
    }
    if (tab_size < 0)
      tab_size= 0;
    else if (tab_size > TAB_SIZE_LIMIT)
      tab_size= TAB_SIZE_LIMIT;
  }

  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr()+js->length());
  je.killed_ptr= (uchar*)&thd->killed;

  if (json_nice(&je, str, fmt, tab_size))
  {
    null_value= 1;
    report_json_error(js, &je, 0);
    thd->check_killed(); // to get the error message right
    return 0;
  }

  return str;
}


String *Item_func_json_format::val_json(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  if ((null_value= args[0]->null_value))
    return 0;
  return js;
}

int Arg_comparator::compare_json_str_basic(Item *j, Item *s)
{
  String *js,*str;
  int c_len;
  json_engine_t je;

  if ((js= j->val_str(&value1)))
  {
    json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                    (const uchar *) js->ptr()+js->length());
     if (json_read_value(&je))
       goto error;
     if (je.value_type == JSON_VALUE_STRING)
     {
       if (value2.realloc_with_extra_if_needed(je.value_len) ||
         (c_len= json_unescape(js->charset(), je.value,
                               je.value + je.value_len,
                               &my_charset_utf8mb3_general_ci,
                               (uchar *) value2.ptr(),
                               (uchar *) (value2.ptr() + je.value_len))) < 0)
         goto error;

       value2.length(c_len);
       js= &value2;
       str= &value1;
     }
     else
     {
       str= &value2;
     }


     if ((str= s->val_str(str)))
     {
       if (set_null)
         owner->null_value= 0;
       return sortcmp(js, str, compare_collation());
     }
  }

error:
  if (set_null)
    owner->null_value= 1;
  return -1;
}


int Arg_comparator::compare_e_json_str_basic(Item *j, Item *s)
{
  String *res1,*res2;
  json_value_types type;
  char *value;
  int value_len, c_len;
  Item_func_json_extract *e= (Item_func_json_extract *) j;

  res1= e->read_json(&value1, &type, &value, &value_len);
  res2= s->val_str(&value2);

  if (!res1 || !res2)
    return MY_TEST(res1 == res2);

  if (type == JSON_VALUE_STRING)
  {
    if (value1.realloc_with_extra_if_needed(value_len) ||
        (c_len= json_unescape(value1.charset(), (uchar *) value,
                              (uchar *) value+value_len,
                              &my_charset_utf8mb3_general_ci,
                              (uchar *) value1.ptr(),
                              (uchar *) (value1.ptr() + value_len))) < 0)
      return 1;
    value1.length(c_len);
    res1= &value1;
  }

  return MY_TEST(sortcmp(res1, res2, compare_collation()) == 0);
}


String *Item_func_json_arrayagg::get_str_from_item(Item *i, String *tmp)
{
  m_tmp_json.length(0);
  if (append_json_value(&m_tmp_json, i, tmp))
    return NULL;
  return &m_tmp_json;
}


String *Item_func_json_arrayagg::get_str_from_field(Item *i,Field *f,
    String *tmp, const uchar *key, size_t offset)
{
  m_tmp_json.length(0);

  if (append_json_value_from_field(&m_tmp_json, i, f, key, offset, tmp))
    return NULL;

  return &m_tmp_json;

}


void Item_func_json_arrayagg::cut_max_length(String *result,
       uint old_length, uint max_length) const
{
  if (result->length() == 0)
    return;

  if (result->ptr()[result->length() - 1] != '"' ||
      max_length == 0)
  {
    Item_func_group_concat::cut_max_length(result, old_length, max_length);
    return;
  }

  Item_func_group_concat::cut_max_length(result, old_length, max_length-1);
  result->append('"');
}


Item *Item_func_json_arrayagg::copy_or_same(THD* thd)
{
   return new (thd->mem_root) Item_func_json_arrayagg(thd, this);
}


String* Item_func_json_arrayagg::val_str(String *str)
{
  if ((str= Item_func_group_concat::val_str(str)))
  {
    String s;
    s.append('[');
    s.swap(*str);
    str->append(s);
    str->append(']');
  }
  return str;
}


Item_func_json_objectagg::
Item_func_json_objectagg(THD *thd, Item_func_json_objectagg *item)
  :Item_sum(thd, item)
{
  quick_group= FALSE;
  result.set_charset(collation.collation);
  result.append('{');
}


bool
Item_func_json_objectagg::fix_fields(THD *thd, Item **ref)
{
  uint i;                       /* for loop variable */
  DBUG_ASSERT(fixed() == 0);

  memcpy(orig_args, args, sizeof(Item*) * arg_count);

  if (init_sum_func_check(thd))
    return TRUE;

  set_maybe_null();

  /*
    Fix fields for select list and ORDER clause
  */

  for (i=0 ; i < arg_count ; i++)
  {
    if (args[i]->fix_fields_if_needed_for_scalar(thd, &args[i]))
      return TRUE;
    with_flags|= args[i]->with_flags;
  }

  /* skip charset aggregation for order columns */
  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return 1;

  result.set_charset(collation.collation);
  result_field= 0;
  null_value= 1;
  max_length= (uint32)(thd->variables.group_concat_max_len
              / collation.collation->mbminlen
              * collation.collation->mbmaxlen);

  if (check_sum_func(thd, ref))
    return TRUE;

  base_flags|= item_base_t::FIXED;
  return FALSE;
}


void Item_func_json_objectagg::cleanup()
{
  DBUG_ENTER("Item_func_json_objectagg::cleanup");
  Item_sum::cleanup();

  result.length(1);
  DBUG_VOID_RETURN;
}


Item *Item_func_json_objectagg::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_func_json_objectagg(thd, this);
}


void Item_func_json_objectagg::clear()
{
  result.length(1);
  null_value= 1;
}


bool Item_func_json_objectagg::add()
{
  StringBuffer<MAX_FIELD_WIDTH> buf;
  String *key;

  key= args[0]->val_str(&buf);
  if (args[0]->is_null())
    return 0;

  null_value= 0;
  if (result.length() > 1)
    result.append(STRING_WITH_LEN(", "));

  result.append('"');
  result.append(*key);
  result.append(STRING_WITH_LEN("\":"));

  buf.length(0);
  append_json_value(&result, args[1], &buf);

  return 0;
}


String* Item_func_json_objectagg::val_str(String* str)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return 0;

  result.append('}');
  return &result;
}


String *Item_func_json_normalize::val_str(String *buf)
{
  String tmp;
  String *raw_json= args[0]->val_str(&tmp);

  DYNAMIC_STRING normalized_json;
  if (init_dynamic_string(&normalized_json, NULL, 0, 0))
  {
    null_value= 1;
    return NULL;
  }

  null_value= args[0]->null_value;
  if (null_value)
    goto end;

  if (json_normalize(&normalized_json,
                     raw_json->ptr(), raw_json->length(),
                     raw_json->charset()))
  {
    null_value= 1;
    goto end;
  }

  buf->length(0);
  if (buf->append(normalized_json.str, normalized_json.length))
  {
    null_value= 1;
    goto end;
  }

end:
  dynstr_free(&normalized_json);
  return null_value ? NULL : buf;
}


bool Item_func_json_normalize::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb4_bin);
  /* 0 becomes 0.0E0, thus one character becomes 5 chars */
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * 5);
  set_maybe_null();
  return FALSE;
}


/*
  When the two values match or don't match we need to return true or false.
  But we can have some more elements in the array left or some more keys
  left in the object that we no longer want to compare. In this case,
  we want to skip the current item.
*/
void json_skip_current_level(json_engine_t *js, json_engine_t *value)
{
  json_skip_level(js);
  json_skip_level(value);
}


/* At least one of the two arguments is a scalar. */
bool json_find_overlap_with_scalar(json_engine_t *js, json_engine_t *value)
{
  if (json_value_scalar(value))
  {
    if (js->value_type == value->value_type)
    {
      if (js->value_type == JSON_VALUE_NUMBER)
      {
        double d_j, d_v;
        char *end;
        int err;

        d_j= js->s.cs->strntod((char *) js->value, js->value_len, &end, &err);
        d_v= value->s.cs->strntod((char *) value->value, value->value_len,
                                   &end, &err);

        return (fabs(d_j - d_v) < 1e-12);
      }
      else if (js->value_type == JSON_VALUE_STRING)
      {
        return value->value_len == js->value_len &&
               memcmp(value->value, js->value, value->value_len) == 0;
      }
    }
    return value->value_type == js->value_type;
  }
  else if (value->value_type == JSON_VALUE_ARRAY)
  {
    while (json_scan_next(value) == 0 && value->state == JST_VALUE)
    {
      if (json_read_value(value))
        return FALSE;
      if (js->value_type == value->value_type)
      {
        int res1= json_find_overlap_with_scalar(js, value);
        if (res1)
          return TRUE;
      }
      if (!json_value_scalar(value))
        json_skip_level(value);
    }
  }
  return FALSE;
}


/*
  Compare when one is object and other is array. This means we are looking
  for the object in the array. Hence, when value type of an element of the
  array is object, then compare the two objects entirely. If they are
  equal return true else return false.
*/
bool json_compare_arr_and_obj(json_engine_t *js, json_engine_t *value)
{
  st_json_engine_t loc_val= *value;
  while (json_scan_next(js) == 0 && js->state == JST_VALUE)
  {
    if (json_read_value(js))
      return FALSE;
    if (js->value_type == JSON_VALUE_OBJECT)
    {
      int res1= json_find_overlap_with_object(js, value, true);
      if (res1)
        return TRUE;
      *value= loc_val;
    }
    if (!json_value_scalar(js))
      json_skip_level(js);
  }
  return FALSE;
}


bool json_compare_arrays_in_order(json_engine_t *js, json_engine_t *value)
{
  bool res= false;
  while (json_scan_next(js) == 0 && json_scan_next(value) == 0 &&
         js->state == JST_VALUE && value->state == JST_VALUE)
  {
    if (json_read_value(js) || json_read_value(value))
      return FALSE;
    if (js->value_type != value->value_type)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
    res= check_overlaps(js, value, true);
    if (!res)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
  }
  res= (value->state == JST_ARRAY_END || value->state == JST_OBJ_END ?
        TRUE : FALSE);
  json_skip_current_level(js, value);
  return res;
}


int json_find_overlap_with_array(json_engine_t *js, json_engine_t *value,
                                 bool compare_whole)
{
  if (value->value_type == JSON_VALUE_ARRAY)
  {
    if (compare_whole)
      return json_compare_arrays_in_order(js, value);

    json_engine_t loc_value= *value, current_js= *js;

    while (json_scan_next(js) == 0 && js->state == JST_VALUE)
    {
      if (json_read_value(js))
        return FALSE;
      current_js= *js;
      while (json_scan_next(value) == 0 && value->state == JST_VALUE)
      {
        if (json_read_value(value))
          return FALSE;
        if (js->value_type == value->value_type)
        {
          int res1= check_overlaps(js, value, true);
          if (res1)
            return TRUE;
        }
        else
        {
          if (!json_value_scalar(value))
            json_skip_level(value);
        }
        *js= current_js;
      }
      *value= loc_value;
      if (!json_value_scalar(js))
        json_skip_level(js);
    }
    return FALSE;
  }
  else if (value->value_type == JSON_VALUE_OBJECT)
  {
    if (compare_whole)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
    return json_compare_arr_and_obj(js, value);
  }
  else
    return json_find_overlap_with_scalar(value, js);
}


int json_find_overlap_with_object(json_engine_t *js, json_engine_t *value,
                                  bool compare_whole)
{
  if (value->value_type == JSON_VALUE_OBJECT)
  {
    /* Find at least one common key-value pair */
    json_string_t key_name;
    bool found_key= false, found_value= false;
    json_engine_t loc_js= *js;
    const uchar *k_start, *k_end;

    json_string_set_cs(&key_name, value->s.cs);

    while (json_scan_next(value) == 0 && value->state == JST_KEY)
    {
      k_start= value->s.c_str;
      do
      {
        k_end= value->s.c_str;
      } while (json_read_keyname_chr(value) == 0);

      if (unlikely(value->s.error))
        return FALSE;

      json_string_set_str(&key_name, k_start, k_end);
      found_key= find_key_in_object(js, &key_name);
      found_value= 0;

      if (found_key)
      {
        if (json_read_value(js) || json_read_value(value))
          return FALSE;

        /*
          The value of key-value pair can be an be anything. If it is an object
          then we need to compare the whole value and if it is an array then
          we need to compare the elements in that order. So set compare_whole
          to true.
        */
        if (js->value_type == value->value_type)
          found_value= check_overlaps(js, value, true);
        if (found_value)
        {
          if (!compare_whole)
            return TRUE;
          *js= loc_js;
        }
        else
        {
          if (compare_whole)
          {
            json_skip_current_level(js, value);
            return FALSE;
          }
          *js= loc_js;
        }
      }
      else
      {
        if (compare_whole)
        {
          json_skip_current_level(js, value);
          return FALSE;
        }
        json_skip_key(value);
        *js= loc_js;
      }
    }
    json_skip_current_level(js, value);
    return compare_whole ? TRUE : FALSE;
  }
  else if (value->value_type == JSON_VALUE_ARRAY)
  {
    if (compare_whole)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
    return json_compare_arr_and_obj(value, js);
  }
  return FALSE;
}


/*
  Find if two json documents overlap

  SYNOPSIS
    check_overlaps()
    js     - json document
    value  - value
    compare_whole - If true then find full overlap with the document in case of
                    object and comparing in-order in case of array.
                    Else find at least one match between two objects or array.

  IMPLEMENTATION
  We can compare two json datatypes if they are of same type to check if
  they are equal. When comparing between a json document and json value,
  there can be following cases:
  1) When at least one of the two json documents is of scalar type:
     1.a) If value and json document both are scalar, then return true
          if they have same type and value.
     1.b) If json document is scalar but other is array (or vice versa),
          then return true if array has at least one element of same type
          and value as scalar.
     1.c) If one is scalar and other is object, then return false because
          it can't be compared.

  2) When both arguments are of non-scalar type:
      2.a) If both arguments are arrays:
           Iterate over the value and json document. If there exists at least
           one element in other array of same type and value as that of
           element in value, then return true else return false.
      2.b) If both arguments are objects:
           Iterate over value and json document and if there exists at least
           one key-value pair common between two objects, then return true,
           else return false.
      2.c) If either of json document or value is array and other is object:
           Iterate over the array, if an element of type object is found,
           then compare it with the object (which is the other arguemnt).
           If the entire object matches i.e all they key value pairs match,
           then return true else return false.

  When we are comparing an object which is nested in other object or nested
  in an array, we need to compare all the key-value pairs, irrespective of
  what order they are in as opposed to non-nested where we return true if
  at least one match is found. However, if we have an array nested in another
  array, then we compare two arrays in that order i.e we compare
  i-th element of array 1 with i-th element of array 2.

  RETURN
    FALSE - If two json documents do not overlap
    TRUE  - if two json documents overlap
*/
int check_overlaps(json_engine_t *js, json_engine_t *value, bool compare_whole)
{
  long arbitrary_var;
  long stack_used_up= (available_stack_size(current_thd->thread_stack, &arbitrary_var));
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  {ALLOCATE_MEM_ON_STACK(my_thread_stack_size-stack_used_up-STACK_MIN_SIZE);});
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  switch (js->value_type)
  {
  case JSON_VALUE_OBJECT:
    return json_find_overlap_with_object(js, value, compare_whole);
  case JSON_VALUE_ARRAY:
    return json_find_overlap_with_array(js, value, compare_whole);
  default:
    return json_find_overlap_with_scalar(js, value);
  }
}

longlong Item_func_json_overlaps::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, ve;
  int result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  json_scan_start(&ve, val->charset(), (const uchar *) val->ptr(),
                  (const uchar *) val->end());

  if (json_read_value(&je) || json_read_value(&ve))
    goto error;

  result= check_overlaps(&je, &ve, false);
  if (unlikely(je.s.error || ve.s.error))
    goto error;

  return result;

error:
  if (je.s.error)
    report_json_error(js, &je, 0);
  if (ve.s.error)
    report_json_error(val, &ve, 1);
  return 0;
}

bool Item_func_json_overlaps::fix_length_and_dec(THD *thd)
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  set_maybe_null();

  return Item_bool_func::fix_length_and_dec(thd);
}

void initialize_curr_constraint(st_json_schema *curr_step)
{
  curr_step->next_step= NULL;
  curr_step->common_constraints.type= JSON_VALUE_UNINITIALIZED;
  curr_step->common_constraints.const_value.has_const= 0;
  curr_step->common_constraints.enum_values.flag= 0;
  curr_step->common_constraints.enum_values.is_enum_inited= 0;

  curr_step->number_constraints.flag= 0;

  curr_step->string_constraints.flag= 0;

  curr_step->array_constraints.flag= 0;
  curr_step->array_constraints.allowed_item_type= JSON_VALUE_UNINITIALIZED;
  curr_step->array_constraints.contains_item_type= JSON_VALUE_UNINITIALIZED;

  curr_step->object_constraints.has_required= 0;
  curr_step->object_constraints.is_properties_inited= 0;

  curr_step->is_initialized= 1;
}

int assign_type(enum json_value_types *curr_type, json_engine_t *je)
{
  if (!strncmp((const char*)je->value, "number", je->value_len))
      *curr_type= JSON_VALUE_NUMBER;
  else if(!strncmp((const char*)je->value, "string", je->value_len))
      *curr_type= JSON_VALUE_STRING;
  else if(!strncmp((const char*)je->value, "array", je->value_len))
      *curr_type= JSON_VALUE_ARRAY;
  else if(!strncmp((const char*)je->value, "object", je->value_len))
      *curr_type= JSON_VALUE_OBJECT;
  else if (!strncmp((const char*)je->value, "true", je->value_len))
      *curr_type= JSON_VALUE_TRUE;
  else if (!strncmp((const char*)je->value, "false", je->value_len))
      *curr_type= JSON_VALUE_FALSE;
  else if (!strncmp((const char*)je->value, "null", je->value_len))
      *curr_type= JSON_VALUE_NULL;
  else
      return true;
  return false;
}

uchar* get_key_name(const char *key_name, size_t *length,
                     my_bool /* unused */)
{
  *length= strlen(key_name);
  return (uchar*) key_name;
}

/*
  Return true if keyword is not handled
*/
int handle_common_constraint_keywords(THD *thd, const char *curr_key,
                                      st_common_constraints
                                            *curr_common_constraint,
                                      json_engine_t *je, int key_len)
{
  if (!strncmp((const char*)curr_key, "type", key_len))
  {
    if (curr_common_constraint->type > JSON_VALUE_UNINITIALIZED ||
        assign_type(&(curr_common_constraint->type), je))
      return true;
  }
  else if(!strncmp((const char*)curr_key, "const", key_len))
  {
    json_engine_t *curr_const_json_value=
                  &curr_common_constraint->const_value.const_json_value;
    int is_scalar= 0, value_len= 0;
    curr_common_constraint->const_value.has_const= 1;
    curr_common_constraint->const_value.const_type= je->value_type;
    if (je->value_type > 2)
    {
      is_scalar= 1;
      value_len= je->value_len;
    }
    const uchar *start= je->value;
    if (!json_value_scalar(je))
      json_skip_level(je);
    json_scan_start(curr_const_json_value, je->s.cs, start, is_scalar ?
                    je->value + value_len : je->s.c_str);
    return false;
  }
  else if (!strncmp((const char*)curr_key, "enum", key_len))
  {
    int curr_level= je->stack_p;
    if (!curr_common_constraint->enum_values.is_enum_inited)
    {
      if (my_hash_init(PSI_INSTRUMENT_ME,
                 &curr_common_constraint->enum_values.enum_number,
                 je->s.cs, 1024, 0, 0, (my_hash_get_key) get_key_name,
                  0, 0) || my_hash_init(PSI_INSTRUMENT_ME,
                 &curr_common_constraint->enum_values.enum_string,
                 je->s.cs, 1024, 0, 0, (my_hash_get_key) get_key_name,
                  0, 0) ||
                 my_hash_init(PSI_INSTRUMENT_ME,
                 &curr_common_constraint->enum_values.enum_array,
                 je->s.cs, 1024, 0, 0, (my_hash_get_key) get_key_name,
                  0, 0) ||
                 my_hash_init(PSI_INSTRUMENT_ME,
                 &curr_common_constraint->enum_values.enum_object,
                 je->s.cs, 1024, 0, 0, (my_hash_get_key) get_key_name,
                  0, 0))
         return true;
      curr_common_constraint->enum_values.is_enum_inited= 1;
    }

    while(json_scan_next(je) == 0 && curr_level <= je->stack_p)
    {
      if (json_read_value(je))
        return true;
      if (je->value_type == JSON_VALUE_NULL)
        curr_common_constraint->enum_values.flag|= HAS_NULL;
      else if (je->value_type == JSON_VALUE_TRUE)
        curr_common_constraint->enum_values.flag|= HAS_TRUE;
      else if (je->value_type == JSON_VALUE_FALSE)
        curr_common_constraint->enum_values.flag|= HAS_FALSE;
      else
      {
        if (json_value_scalar(je))
        {
          char *val= (char*)alloc_root(thd->mem_root, sizeof(je->value_len));
          val[je->value_len]= '\0';
          strncpy(val, (const char*)je->value, je->value_len);
          if (je->value_type == JSON_VALUE_NUMBER)
          {
            if (my_hash_insert(
                   &curr_common_constraint->enum_values.enum_number,
                   (const uchar*)(val)))
             return true;
          }
          else
          {
            if (my_hash_insert(
                   &curr_common_constraint->enum_values.enum_string,
                   (const uchar*)(val)))
             return true;
          }
        }
        else
        {
          char *val_begin= (char*)je->value;
          if (json_skip_level(je))
            return true;
          char *val_end= (char *)je->s.c_str;
          char *val= (char*)malloc(val_end-val_begin);
          val[val_end-val_begin]= '\0';
          strncpy(val, (const char*)je->value, val_end-val_begin);
          DYNAMIC_STRING a_res;
          if (init_dynamic_string(&a_res, NULL, 0, 0))
          {
            dynstr_free(&a_res);
            return true;
          }
          if (json_normalize(&a_res, (const char*)val, val_end-val_begin, je->s.cs))
          {
            dynstr_free(&a_res);
            return true;
          }
          char *temp= (char*)alloc_root(thd->mem_root, a_res.length);
          temp[a_res.length]= '\0';
          strncpy(temp, (const char*)a_res.str, a_res.length);
          if (je->value_type == JSON_VALUE_ARRAY)
          {
           if (my_hash_insert(
                   &curr_common_constraint->enum_values.enum_array,
                   (const uchar*)(temp)))
             return true; 
          }
          else
          {
            if (my_hash_insert(
                   &curr_common_constraint->enum_values.enum_object,
                   (const uchar*)(temp)))
             return true;
          }
          dynstr_free(&a_res);
        }
      }
    }
  }
  else
   return true;

  return false;
}

/*
  Return true if keyword not handled
*/
int handle_number_constraint_keyword(const char *curr_key,
                                     st_number_constraints
                                                      *curr_number_constraint,
                                   double val, int key_len,
                                   st_common_constraints
                                                      *curr_common_constraint)
{
  int res= 0;

  if (!strncmp((const char*)curr_key, "maximum", key_len))
  {
    curr_number_constraint->max= val;
    curr_number_constraint->flag|= HAS_MAX;
  }
  else if (!strncmp((const char*)curr_key, "minimum", key_len))
  {
    curr_number_constraint->min= val;
    curr_number_constraint->flag|= HAS_MIN;
  }
  else if (!strncmp((const char*)curr_key, "multipleOf", key_len) && val > 0)
  {
    /*
     value of "multipleOf" should be >= 0 according to rules for json schema
    */
    curr_number_constraint->multiple_of= val;
    curr_number_constraint->flag|= HAS_MULTIPLE_OF;
  }
  else if (!strncmp((const char*)curr_key, "exclusiveMaximum", key_len))
  {
    curr_number_constraint->ex_max= val;
    curr_number_constraint->flag|= HAS_EXCLUSIVE_MAX;
  }
  else if (!strncmp((const char*)curr_key, "exclusiveMinimum", key_len))
  {
    curr_number_constraint->ex_min= val;
    curr_number_constraint->flag|= HAS_EXCLUSIVE_MIN;
  }
  else
    res= 1;
  return res;
}

/*
  Return true if keyword not handled
*/
int handle_string_constraint_keyword(const char *curr_key,
                                     st_string_constraints
                                                      *curr_string_constraint,
                                     double val, int key_len,
                                     st_common_constraints
                                                      *curr_common_constraint)
{
  int res= 0;

  if (!strncmp((const char*)curr_key, "maxLength", key_len))
  {
    curr_string_constraint->max_len= val;
    curr_string_constraint->flag|= HAS_MAX_LEN;
  }
  else if (!strncmp((const char*)curr_key, "minLength", key_len))
  {
    curr_string_constraint->min_len= val;
    curr_string_constraint->flag|= HAS_MIN_LEN;
  }
  else
    res= true;

  return res;
}

/*
  Return true if keyword not handled
*/
int handle_array_constraint_keyword(const char *curr_key,
                                    st_array_constraints
                                                       *curr_array_constraint,
                                    double val, int key_len,
                                    st_common_constraints
                                                      *curr_common_constraint,
                                    json_engine_t *je)
{
  int res= 0;

  if (!strncmp((const char*)curr_key, "maxItems", key_len))
  {
    curr_array_constraint->max_items= val;
    curr_array_constraint->flag|= HAS_MAX_ITEMS;
  }
  else if (!strncmp((const char*)curr_key, "minItems", key_len))
  {
    curr_array_constraint->min_items= val;
    curr_array_constraint->flag|= HAS_MIN_ITEMS;
  }
  else if(!strncmp((const char*)curr_key, "items", key_len))
  {
    if (curr_common_constraint->type == JSON_VALUE_ARRAY ||
       curr_common_constraint->type == JSON_VALUE_UNINITIALIZED)
    {
      if (json_scan_next(je) || json_read_value(je) ||
          assign_type(&(curr_array_constraint->allowed_item_type), je))
        return true;
    }
  }
  else if (!strncmp((const char*)curr_key, "contains", key_len))
  {
    if (json_read_value(je) || je->value_type != JSON_VALUE_OBJECT)
      return 1;
    int curr_level= je->stack_p;
    while(json_scan_next(je) && je->stack_p >= curr_level-1)
    {
      const uchar *key_end, *key_start= je->s.c_str;
      do
      {
        key_end= je->s.c_str;
      } while (json_read_keyname_chr(je) == 0);

      if (strncmp((const char*)"type", (const char*)key_start, key_end-key_start))
        return 1;
      if (json_scan_next(je) || json_read_value(je) ||
          assign_type(&(curr_array_constraint->contains_item_type), je))
        return true; 
    }
  }
  else if (!strncmp((const char*)curr_key, "maxContains", key_len))
  {
    curr_array_constraint->max_contains= val;
    curr_array_constraint->flag|= HAS_MAX_CONTAINS;
  }
  else if (!strncmp((const char*)curr_key, "minContains", key_len))
  {
    curr_array_constraint->min_contains= val;
    curr_array_constraint->flag|= HAS_MIN_CONTAINS;
  }
  else
    res= true;

  return res;
}

int fill_curr_property(THD *thd,
                       json_engine_t *je, int *count,
                       st_json_schema *next_step, st_object_property *curr_step)
{
  int level= je->stack_p;
  while (json_scan_next(je)== 0 && je->stack_p >= level)
  {
    int flag= 0;
     switch(je->state)
     {
       case JST_KEY:
       {
          const uchar *key_end, *key_start= je->s.c_str;
          do
          {
            key_end= je->s.c_str;
          } while (json_read_keyname_chr(je) == 0);

          if (json_read_value(je))
            return true;
          if (!strncmp((const char*)key_start, "properties", 10))
            flag= 1;

          if (handle_keyword(thd, (const char*)key_start,
                             flag ? next_step : &(curr_step->keyname_constraints), je,
                            key_end - key_start, count))
            return true;
          break;
       }
     }
  }
  return false;
}

uchar* get_key_for_property(const uchar *buff, size_t *length,
                        my_bool /* unused */)
{
  st_object_property *curr_obj_property = (st_object_property*) buff;

  *length= curr_obj_property->keyname.length;
  return (uchar*) curr_obj_property->keyname.str;
}

int handle_object_constraint_keyword(THD *thd, const char *curr_key,
                                   st_json_schema *curr_step,
                                   double val, int key_len,
                                   st_common_constraints
                                         *curr_common_constraint,
                                   json_engine_t *je, int *count)
{
  int res= false, level= je->stack_p;

  if (!strncmp((const char*)curr_key, "properties", key_len))
  {
    while(json_scan_next(je)==0 && level <= je->stack_p)
    {
      switch(je->state)
      {
        case JST_KEY:
        {
          const uchar *key_end, *key_start= je->s.c_str;
          do
          {
            key_end= je->s.c_str;
          } while (json_read_keyname_chr(je) == 0);

          if (json_read_value(je))
            return true;
          st_object_property *curr_obj_property=
             (st_object_property *) alloc_root(thd->mem_root,
                                             sizeof(st_object_property));
          initialize_curr_constraint(&(curr_obj_property->keyname_constraints));
          curr_obj_property->keyname.str=
                        (const char *)alloc_root(thd->mem_root, key_end-key_start);
          strncpy((char*)curr_obj_property->keyname.str,
                  (const char*)key_start,key_end-key_start);
          curr_obj_property->keyname.length= key_end-key_start;
          curr_obj_property->keyname_constraints.next_step=
                    (st_json_schema*)alloc_root(thd->mem_root, sizeof(st_json_schema));
          initialize_curr_constraint(curr_obj_property->keyname_constraints.next_step);
          
          if (fill_curr_property(thd,
                       je, count,
                       curr_obj_property->keyname_constraints.next_step, curr_obj_property))
              return true;

          if (!curr_step->object_constraints.is_properties_inited)
          {
            if (my_hash_init(PSI_INSTRUMENT_ME,
                             &curr_step->object_constraints.properties,
                             je->s.cs, 1024, 0, 0,
                             (my_hash_get_key) get_key_for_property,
                             0, HASH_UNIQUE))
              return true;
            curr_step->object_constraints.is_properties_inited= 1;
          }
          if (my_hash_insert(&(curr_step->object_constraints.properties),
                            (const uchar*)(curr_obj_property)))
            return true;
          break;
        }
      }
    }
  }
  else if (!strncmp((const char*)curr_key, "required", key_len))
  {
    curr_step->object_constraints.has_required= 1;
    curr_step->object_constraints.required_properties.s.c_str= je->value;
    if (json_skip_level(je))
      return 1;
    curr_step->object_constraints.required_properties.s.str_end= je->s.c_str;
    curr_step->object_constraints.required_properties.s.cs= je->s.cs;
  }
  return res;
}

/*
 Return true if keyword not handled
*/
int handle_keyword(THD *thd, const char *curr_key, st_json_schema *curr_step,
                   json_engine_t *je, int key_len, int *count)
{
  int res= 1, err;
  char *end;
  double val= je->s.cs->strntod((char *) je->value, je->value_len, &end,
                                &err);

  if (res && (!strncmp((const char*)curr_key, "title", key_len) ||
              !strncmp((const char*)curr_key, "description", key_len) ||
              !strncmp((const char*)curr_key, "$comment", key_len)))
  {
    /*Skip the values for these because we are not going to "validate" them. */
    res= 0;
    if (json_read_value(je) || je->value_type != JSON_VALUE_STRING)
      return 1;
  }
  if (res && (!strncmp((const char*)curr_key, "type", key_len) ||
      !strncmp((const char*)curr_key, "const", key_len) ||
      !strncmp((const char*)curr_key, "enum", key_len)))
    res= handle_common_constraint_keywords(thd, curr_key,
                                           &(curr_step->common_constraints),
                                           je, key_len);
  if (res && (curr_step->common_constraints.type == JSON_VALUE_UNINITIALIZED ||
        curr_step->common_constraints.type == JSON_VALUE_NUMBER))
    res= handle_number_constraint_keyword(curr_key,
                                          &(curr_step->number_constraints),
                                          val, key_len,
                                          &curr_step->common_constraints);
  if (res && (curr_step->common_constraints.type == JSON_VALUE_UNINITIALIZED ||
           curr_step->common_constraints.type == JSON_VALUE_STRING))
    res= handle_string_constraint_keyword(curr_key,
                                          &(curr_step->string_constraints),
                                          val, key_len,
                                          &curr_step->common_constraints);
  if (res && (curr_step->common_constraints.type == JSON_VALUE_UNINITIALIZED ||
           curr_step->common_constraints.type == JSON_VALUE_ARRAY))
    res= handle_array_constraint_keyword(curr_key,
                                         &(curr_step->array_constraints),
                                         val, key_len,
                                         &curr_step->common_constraints, je);
  if (res && (curr_step->common_constraints.type == JSON_VALUE_UNINITIALIZED ||
           curr_step->common_constraints.type == JSON_VALUE_OBJECT))
    res= handle_object_constraint_keyword(thd, curr_key, curr_step,
                                          val, key_len,
                                          &curr_step->common_constraints,
                                          je, count);
  /* Send error about keyword not handled. */
  return res;
}


/*
  Return true if can't validate
*/
int validate_scalar(json_engine_t *je, st_json_schema *curr_step)
{
   st_const_value *curr_const_value=
                      &curr_step->common_constraints.const_value;
   json_engine_t  *curr_step_json= &(curr_const_value->const_json_value);
   int res= 1, enum_validated= true;

   uint enum_flag= curr_step->common_constraints.enum_values.flag;
   if ((enum_flag & je->value_type))
     enum_validated= true;
   if (curr_step->common_constraints.enum_values.is_enum_inited)
   {
    char *temp= (char*)malloc(je->value_len);
    temp[je->value_len]= '\0';
    strncpy(temp, (const char*)je->value, je->value_len);
    char *val_found;
    if (je->value_type == JSON_VALUE_NUMBER)
    {
      if (!(val_found= (char*)my_hash_search(
            &curr_step->common_constraints.enum_values.enum_number,
            (const uchar*)temp, strlen(temp))))
      {
        enum_validated= false;
      }
    }
    else if (je->value_type == JSON_VALUE_STRING)
    {
      if (!(val_found= (char*)my_hash_search(
            &curr_step->common_constraints.enum_values.enum_string,
            (const uchar*)temp, strlen(temp))))
      {
        enum_validated= false;
      }
    }
    else
     return false;
  }
  if (!enum_validated)
    return true;

  if (je->value_type == JSON_VALUE_NUMBER)
  {
    char *end;
    int err;
    uint curr_flag= curr_step->number_constraints.flag;

    double val= je->s.cs->strntod((char *) je->value,
                                  je->value_len, &end, &err);
    double temp= val / curr_step->number_constraints.multiple_of;
    double is_multiple_of= curr_flag & HAS_MULTIPLE_OF ?
                (temp - (long long int)temp) == 0 : true;
    double const_value;
    if (curr_step->common_constraints.const_value.has_const)
    {
     const_value=
             curr_step_json->s.cs->strntod((char *)curr_step_json->s.c_str,
              curr_step_json->s.str_end-curr_step_json->s.c_str, &end, &err);
    }
    if ((curr_step->common_constraints.type == JSON_VALUE_NUMBER) &&
        (curr_flag & HAS_MAX ?
         val <= curr_step->number_constraints.max : true) &&
        (curr_flag & HAS_EXCLUSIVE_MAX ?
         val < curr_step->number_constraints.ex_max : true) &&
        (curr_flag & HAS_MIN ?
         val >= curr_step->number_constraints.min : true) &&
        (curr_flag & HAS_EXCLUSIVE_MIN ?
         val > curr_step->number_constraints.ex_min : true) &&
        (curr_flag & HAS_MULTIPLE_OF ? is_multiple_of : true) &&
        (curr_step->common_constraints.const_value.has_const ?
         val == const_value : true))
      res= false;
  }
  else if(je->value_type == JSON_VALUE_STRING)
  {
    bool str_const_validated= false;
    if (curr_step->common_constraints.const_value.has_const)
    {
      if (je->value_len == (curr_step_json->s.str_end-curr_step_json->s.c_str)
          && !strncmp((const char*)curr_step_json->s.c_str,
                   (const char*)je->value, je->value_len))
        str_const_validated= true;
    }
    uint curr_flag= curr_step->string_constraints.flag;
    if ((curr_step->common_constraints.type == JSON_VALUE_STRING) &&
        (curr_flag & HAS_MAX_LEN ?
         je->value_len <= curr_step->string_constraints.max_len : true) &&
        (curr_flag & HAS_MIN_LEN ?
         je->value_len >= curr_step->string_constraints.min_len : true) &&
        (curr_step->common_constraints.const_value.has_const ?
                                       str_const_validated : true))
      res= false;
  }
  else
  {
    /*
      If value_type is NULL, TRUE, or FALSE. We have already validated it
      before calling validate_scalar() so just set res to false.
    */
    res= false;
  }

  return res;
}

bool validate_array(json_engine_t *je,
                        json_engine_t *const_je,
                         st_json_schema *curr_step)
{
  int number_of_elements= 0, level, contains= 0;
  bool contains_validated= false;
  st_array_constraints *curr_constraint_arr= &curr_step->array_constraints;

  json_engine_t temp_je= *je;
  level= temp_je.stack_p;

  while (json_scan_next(&temp_je) == 0 && je->stack_p >= level &&
           json_read_value(&temp_je)== 0)
  {
    if (curr_constraint_arr->allowed_item_type != JSON_VALUE_UNINITIALIZED &&
        (curr_constraint_arr->allowed_item_type != temp_je.value_type))
      return true;
    if (curr_constraint_arr->contains_item_type != JSON_VALUE_UNINITIALIZED &&
             curr_constraint_arr->contains_item_type == temp_je.value_type)
    {
      contains_validated= true;
      contains++;
    }
      contains_validated= true;
    if (temp_je.stack_p < level)
      break;
    if (!json_value_scalar(&temp_je))
      json_skip_level(&temp_je);
    number_of_elements++;
  }
  if (curr_constraint_arr->contains_item_type != JSON_VALUE_UNINITIALIZED &&
      !contains_validated)
    return false;
  if ((curr_constraint_arr->contains_item_type != JSON_VALUE_UNINITIALIZED) &&
      (curr_constraint_arr->flag & HAS_MAX_CONTAINS ?
       contains <= curr_constraint_arr->max_contains : true) &&
      (curr_constraint_arr->flag & HAS_MIN_CONTAINS ?
       contains >= curr_constraint_arr->max_contains : true))
    return false;

  if (curr_step->common_constraints.const_value.has_const)
  {
    if (json_read_value(const_je))
      return true;
    if (!json_compare_arrays_in_order(je, const_je))
     return true;
  }
  if ((curr_constraint_arr->flag & HAS_MAX_ITEMS ?
       number_of_elements <= curr_constraint_arr->max_items : true) &&
       (curr_constraint_arr->flag & HAS_MIN_ITEMS ?
        number_of_elements >= curr_constraint_arr->min_items : true))
    return false;
  return true;
}

bool validate_object(json_engine_t *je,
                        json_engine_t *const_je,
                         st_json_schema *curr_step, st_json_schema *new_step)
{
  int curr_level= je->stack_p;
  HASH temp;
  json_engine_t required_arr;
  if (curr_step->object_constraints.has_required)
  {
    my_hash_init(PSI_INSTRUMENT_ME, &temp,
               je->s.cs, 1024, 0, 0, (my_hash_get_key) get_key_name,
               my_free, 0);

    json_scan_start(&required_arr, je->s.cs,
    (const uchar *) curr_step->object_constraints.required_properties.s.c_str,
    (const uchar *) curr_step->object_constraints.required_properties.s.str_end);
  }
  if (new_step ? new_step->common_constraints.const_value.has_const : curr_step->common_constraints.const_value.has_const)
  {
    json_engine_t temp_je, to_skip= *je;
    if (json_skip_level(&to_skip))
      return true;
    json_scan_start(&temp_je, je->s.cs, je->value, to_skip.s.c_str);
    if (json_read_value(&temp_je) || json_read_value(const_je))
      return true;
    if (!check_overlaps(&temp_je, const_je, true))
      return true;
  }

  while (json_scan_next(je)== 0 && je->stack_p >= curr_level)
  {
    switch (je->state)
    {
      case JST_KEY:
      {
        const uchar *key_end, *key_start= je->s.c_str;
        st_object_property *curr_obj_property;
        do
        {
          key_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);

        if (json_read_value(je))
          return true;

        if (curr_step->object_constraints.has_required)
        {
          char *str= (char*)alloc_root(current_thd->mem_root, key_end-key_start);
          str[(int)(key_end-key_start)]='\0';
          strncpy((char*)str, (const char*)key_start, key_end-key_start);

          if (my_hash_insert(&temp, (uchar*)(str)))
           return true;
        }

        if (curr_step->object_constraints.is_properties_inited)
        {
          if ((curr_obj_property = (st_object_property*) my_hash_search(
                                &curr_step->object_constraints.properties,
                                (const uchar*) key_start,
                                key_end-key_start)))
          {
            if (curr_obj_property->keyname_constraints.common_constraints.type
                != je->value_type)
             return true;
            int res= 0;
            if (curr_obj_property->keyname_constraints.common_constraints.type > 2)
            {
              res= validate_scalar(je,
                                    &(curr_obj_property->keyname_constraints));
            }
            else
            {
              st_common_constraints *curr_common_constraints=
                &(curr_obj_property->keyname_constraints.common_constraints);
             
              if (curr_obj_property->keyname_constraints.common_constraints.type
                  ==JSON_VALUE_ARRAY)
              {
                res= validate_array(je, curr_common_constraints->const_value.has_const ?
                                    &(curr_common_constraints->const_value.const_json_value) :
                                    const_je, &(curr_obj_property->keyname_constraints));
              }
              else
              {
                res= validate_object(je, curr_common_constraints->const_value.has_const ?
                                    &(curr_common_constraints->const_value.const_json_value) :
                                    const_je, curr_obj_property->keyname_constraints.next_step, &(curr_obj_property->keyname_constraints));
              }
            }
            if (res)
              return true;
          }
        else
          json_skip_level(je);
        }
        break;
      }
    }
  }
  if (curr_step->object_constraints.has_required)
  {
    LEX_CSTRING *temp_key_word;
    if (json_scan_next(&required_arr))
      return true;
    int curr_level= (&required_arr)->stack_p;
    while(json_scan_next(&required_arr) == 0 &&
          (&required_arr)->stack_p >= curr_level)
    {
      if (json_read_value(&required_arr))
        return 1;
      if (!(temp_key_word= (LEX_CSTRING*) my_hash_search( &temp,
                                        (const uchar*) (&required_arr)->value,
                                        (&required_arr)->value_len)))
      {
        my_hash_free(&temp);
        return true;
      }
    }
  }
  my_hash_free(&temp);
  return false;
}

/*
 Return true if does not validate
*/
int validate_non_scalar(json_engine_t *je,
                        json_engine_t *const_je,
                         st_json_schema *curr_step,
                         st_json_schema *next_step)
{
  if(curr_step->common_constraints.enum_values.is_enum_inited)
  {
    json_engine_t temp_je= *je;
    char *val_begin= (char*)temp_je.value;
    if (json_skip_level(&temp_je))
      return true;
    char *val_end= (char*)temp_je.s.c_str;
    char *val= (char*)malloc(val_end-val_begin);
    strncpy(val, (const char*)val_begin, val_end-val_begin);
    DYNAMIC_STRING a_res;
    char *res;
    if (init_dynamic_string(&a_res, NULL, 0, 0))
    {
      dynstr_free(&a_res);
      return true;
    }
    if (json_normalize(&a_res, (const char*)val, val_end-val_begin,
                        je->s.cs))
    {
      dynstr_free(&a_res);
      return true;
    }
    if (je->value_type == JSON_VALUE_ARRAY)
    {
      if (!(res=(char*)my_hash_search(
            &curr_step->common_constraints.enum_values.enum_array,
            (const uchar*)a_res.str, a_res.length)))
      {
       dynstr_free(&a_res);
      return true;
      }
    }
    else
    {
      if (!(res=(char*)my_hash_search(
            &curr_step->common_constraints.enum_values.enum_object,
            (const uchar*)a_res.str, a_res.length)))
      {
        dynstr_free(&a_res);
      return true;
      }
    }
    dynstr_free(&a_res);
  }
  if (je->value_type == JSON_VALUE_ARRAY)
    return validate_array(je, const_je, curr_step);
  else if (je->value_type == JSON_VALUE_OBJECT)
    return validate_object(je, const_je, curr_step, NULL);
  return false;
}

/*
Return true if not validated
*/
int validate_against_schema(json_engine_t *je,
                             st_json_schema *curr_step,
                             int *count)
{
  json_engine_t *const_je;
  if (curr_step->common_constraints.const_value.has_const)
    const_je= &(curr_step->common_constraints.const_value.const_json_value);
  if (json_read_value(je) ||
      je->value_type != curr_step->common_constraints.type)
    return true;

  if (json_value_scalar(je))
  {
    if (validate_scalar(je, curr_step))
      return true;
  }
  else
  {
    if (validate_non_scalar(je, const_je, curr_step, NULL))
      return true;
  }
  return false;
}

/*
  Return true if error or does not validate
*/
int validate_json_schema(THD *thd, json_engine_t *je,
                         st_json_schema *curr_step, int *count)
{
 initialize_curr_constraint(curr_step);
 curr_step->is_initialized= 1;

  int level= je->stack_p;
  while (je->s.c_str < je->s.str_end && json_scan_next(je)== 0 && je->stack_p >= level)
  {
     switch(je->state)
     {
       case JST_KEY:
       {
          const uchar *key_end, *key_start= je->s.c_str;
          do
          {
            key_end= je->s.c_str;
          } while (json_read_keyname_chr(je) == 0);

          if (json_read_value(je))
            return true;
          if (handle_keyword(thd, (const char*)key_start,
                            curr_step, je,
                            key_end - key_start, count))
            return true;
          break;
       }
     }
  }
  return false;
}

longlong Item_func_json_schema_valid::val_int()
{
  json_engine_t ve;
  int count= 0, res= 1;

  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  json_scan_start(&ve, val->charset(), (const uchar *) val->ptr(),
                  (const uchar *) val->end());

  if (validate_against_schema(&ve, curr_step, &count))
    res= 0;
  if (unlikely(ve.s.error))
    res= 0;
  
  st_json_schema *temp_curr_step= &(curr_step[0]);
  do
  {
    if (temp_curr_step->common_constraints.type > JSON_VALUE_UNINITIALIZED)
    {
      if (temp_curr_step->common_constraints.enum_values.is_enum_inited)
      {
        st_enum *curr_enum= &(temp_curr_step->common_constraints.enum_values);
        if (curr_enum->is_enum_inited)
        {
          my_hash_free(&curr_enum->enum_number);
          my_hash_free(&curr_enum->enum_string);
          my_hash_free(&curr_enum->enum_array);
          my_hash_free(&curr_enum->enum_object);
        }
      }
      if (temp_curr_step->common_constraints.type == JSON_VALUE_OBJECT)
      {
        st_object_constraints *curr_obj= &(temp_curr_step->object_constraints);
        if (curr_obj->is_properties_inited)
          my_hash_free(&curr_obj->properties);
      }
    }
    temp_curr_step= temp_curr_step->next_step;
  }while(temp_curr_step);
 
  if (ve.s.error)
    report_json_error(val, &ve, 1);
  return res;
}

bool Item_func_json_schema_valid::fix_length_and_dec(THD *thd)
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  set_maybe_null();

  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  int res= 0;

  if ((null_value= args[0]->null_value))
    return 0;
  res= json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  res= validate_json_schema(thd, &je, curr_step, &count);

  if (je.s.error)
   res= 1;

  return res || Item_bool_func::fix_length_and_dec(thd);
}
