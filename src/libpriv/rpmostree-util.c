/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <gio/gunixoutputstream.h>

#include "rpmostree-util.h"
#include "rpmostree-origin.h"
#include "rpmostree.h"
#include "libglnx.h"

int
rpmostree_ptrarray_sort_compare_strings (gconstpointer ap,
                                         gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

GVariant *
_rpmostree_vardict_lookup_value_required (GVariantDict *dict,
                                          const char *key,
                                          const GVariantType *fmt,
                                          GError     **error)
{
  GVariant *r = g_variant_dict_lookup_value (dict, key, fmt);
  if (!r)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Failed to find metadata key %s (signature %s)", key, (char*)fmt);
      return NULL;
    }
  return r;
}

gboolean
rpmostree_mkdtemp (const char   *template,
                   char        **out_tmpdir,
                   int          *out_tmpdir_dfd,  /* allow-none */
                   GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *tmpdir = g_strdup (template);
  gboolean created_tmpdir = FALSE;
  glnx_fd_close int ret_tmpdir_dfd = -1;

  if (mkdtemp (tmpdir) == NULL)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  created_tmpdir = TRUE;

  if (out_tmpdir_dfd)
    {
      if (!glnx_opendirat (AT_FDCWD, tmpdir, FALSE, &ret_tmpdir_dfd, error))
        goto out;
    }

  ret = TRUE;
  *out_tmpdir = g_steal_pointer (&tmpdir);
  if (out_tmpdir_dfd)
    {
      *out_tmpdir_dfd = ret_tmpdir_dfd;
      ret_tmpdir_dfd = -1;
    }
 out:
  if (created_tmpdir && tmpdir)
    {
     (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmpdir, NULL, NULL);
    }
  return ret;
}

/* Given a string of the form
 * "bla blah ${foo} blah ${bar}"
 * and a hash table of variables, substitute the variable values.
 */
char *
_rpmostree_varsubst_string (const char *instr,
                            GHashTable *substitutions,
                            GError **error)
{
  const char *s;
  const char *p;
  /* Acts as a reusable buffer space */
  g_autoptr(GString) varnamebuf = g_string_new ("");
  g_autoptr(GString) result = g_string_new ("");

  s = instr;
  while ((p = strstr (s, "${")) != NULL)
    {
      const char *varstart = p + 2;
      const char *varend = strchr (varstart, '}');
      const char *value;
      if (!varend)
        return glnx_null_throw (error, "Unclosed variable reference in %s starting at %u bytes",
                                instr, (guint)(p - instr));

      /* Append leading bytes */
      g_string_append_len (result, s, p - s);

      /* Get a NUL-terminated copy of the variable name */
      g_string_truncate (varnamebuf, 0);
      g_string_append_len (varnamebuf, varstart, varend - varstart);

      value = g_hash_table_lookup (substitutions, varnamebuf->str);
      if (!value)
        return glnx_null_throw (error, "Unknown variable reference ${%s} in %s",
                                varnamebuf->str, instr);
      /* Append the replaced value */
      g_string_append (result, value);

      /* On to the next */
      s = varend+1;
    }

  /* Append trailing bytes */
  if (s != instr)
    {
      g_string_append (result, s);
      /* Steal the C string, NULL out the GString since we freed it */
      return g_string_free (g_steal_pointer (&result), FALSE);
    }
  else
    return g_strdup (instr);
}

gboolean
_rpmostree_util_update_checksum_from_file (GChecksum    *checksum,
                                           int           dfd,
                                           const char   *path,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  glnx_fd_close int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;

  fd = openat (dfd, path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return FALSE;

  g_checksum_update (checksum, (guint8*)g_mapped_file_get_contents (mfile),
                     g_mapped_file_get_length (mfile));

  return TRUE;
}

static char *
ost_get_prev_commit (OstreeRepo *repo, char *checksum)
{
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, NULL))
    return NULL;

  return ostree_commit_get_parent (commit);
}

GPtrArray *
_rpmostree_util_get_commit_hashes (OstreeRepo    *repo,
                                   const char    *beg,
                                   const char    *end,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  GPtrArray *ret = NULL;
  g_autofree char *beg_checksum = NULL;
  g_autofree char *end_checksum = NULL;
  g_autofree char *parent = NULL;
  char *checksum = NULL;
  gboolean worked = FALSE;

  if (!ostree_repo_read_commit (repo, beg, NULL, &beg_checksum, cancellable, error))
    goto out;

  ret = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (ret, g_strdup (beg));  /* Add the user defined REFSPEC. */

  if (end &&
      !ostree_repo_read_commit (repo, end, NULL, &end_checksum, cancellable, error))
      goto out;

  if (end && g_str_equal (end_checksum, beg_checksum))
      goto worked_out;

  checksum = beg_checksum;
  while ((parent = ost_get_prev_commit (repo, checksum)))
    {
      if (end && g_str_equal (end_checksum, parent))
        { /* Add the user defined REFSPEC. */
          g_ptr_array_add (ret, g_strdup (end));
          break;
        }

      g_ptr_array_add (ret, parent);
      checksum = parent;
    }

  if (end && !parent)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid ref range: %s is not a parent of %s", end, beg);
      goto out;
    }

 worked_out:
  worked = TRUE;

 out:
  if (!worked)
    {
      g_ptr_array_free (ret, TRUE);
      ret = NULL;
    }
  return ret;
}

char *
_rpmostree_util_next_version (const char *auto_version_prefix,
                              const char *last_version)
{
  unsigned long long num = 0;
  const char *end = NULL;

  if (!last_version || !g_str_has_prefix (last_version, auto_version_prefix))
    return g_strdup (auto_version_prefix);

  if (g_str_equal (last_version, auto_version_prefix))
    return g_strdup_printf ("%s.1", auto_version_prefix);

  end = last_version + strlen(auto_version_prefix);

  if (*end != '.')
    return g_strdup (auto_version_prefix);
  ++end;

  num = g_ascii_strtoull (end, NULL, 10);
  return g_strdup_printf ("%s.%llu", auto_version_prefix, num + 1);
}

/* Replace every occurrence of @old in @buf with @new. */
char *
rpmostree_str_replace (const char  *buf,
                       const char  *old,
                       const char  *new,
                       GError     **error)
{
  g_autofree char *literal_old = g_regex_escape_string (old, -1);
  g_autoptr(GRegex) regex = g_regex_new (literal_old, 0, 0, error);

  if (regex == NULL)
    return NULL;

  return g_regex_replace_literal (regex, buf, -1, 0, new, 0, error);
}

static gboolean
pull_content_only_recurse (OstreeRepo  *dest,
                           OstreeRepo  *src,
                           OstreeRepoCommitTraverseIter *iter,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean done = FALSE;

  while (!done)
    {
      OstreeRepoCommitIterResult iterres =
        ostree_repo_commit_traverse_iter_next (iter, cancellable, error);

      switch (iterres)
        {
        case OSTREE_REPO_COMMIT_ITER_RESULT_ERROR:
          return FALSE;
        case OSTREE_REPO_COMMIT_ITER_RESULT_END:
          done = TRUE;
          break;
        case OSTREE_REPO_COMMIT_ITER_RESULT_FILE:
          {
            char *name;
            char *checksum;

            ostree_repo_commit_traverse_iter_get_file (iter, &name, &checksum);

            if (!ostree_repo_import_object_from (dest, src, OSTREE_OBJECT_TYPE_FILE,
                                                 checksum, cancellable, error))
              return FALSE;
          }
          break;
        case OSTREE_REPO_COMMIT_ITER_RESULT_DIR:
          {
            char *name;
            char *content_checksum;
            char *meta_checksum;
            g_autoptr(GVariant) dirtree = NULL;
            ostree_cleanup_repo_commit_traverse_iter
              OstreeRepoCommitTraverseIter subiter = { 0, };

            ostree_repo_commit_traverse_iter_get_dir (iter, &name, &content_checksum, &meta_checksum);

            if (!ostree_repo_load_variant (src, OSTREE_OBJECT_TYPE_DIR_TREE,
                                           content_checksum, &dirtree,
                                           error))
              return FALSE;

            if (!ostree_repo_commit_traverse_iter_init_dirtree (&subiter, src, dirtree,
                                                                OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                                error))
              return FALSE;

            if (!pull_content_only_recurse (dest, src, &subiter, cancellable, error))
              return FALSE;
          }
          break;
        }
    }

  return TRUE;
}

/* Migrate only the content (.file) objects from src+src_commit into dest.
 * Used for package layering.
 */
gboolean
rpmostree_pull_content_only (OstreeRepo  *dest,
                             OstreeRepo  *src,
                             const char  *src_commit,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GVariant) commitdata = NULL;
  ostree_cleanup_repo_commit_traverse_iter
    OstreeRepoCommitTraverseIter iter = { 0, };

  if (!ostree_repo_load_commit (src, src_commit, &commitdata, NULL, error))
    return FALSE;

  if (!ostree_repo_commit_traverse_iter_init_commit (&iter, src, commitdata,
                                                     OSTREE_REPO_COMMIT_TRAVERSE_FLAG_NONE,
                                                     error))
    return FALSE;

  if (!pull_content_only_recurse (dest, src, &iter, cancellable, error))
    return FALSE;

  return TRUE;
}

G_LOCK_DEFINE_STATIC (pathname_cache);

/**
 * rpmostree_file_get_path_cached:
 *
 * Like g_file_get_path(), but returns a constant copy so callers
 * don't need to free the result.
 */
const char *
rpmostree_file_get_path_cached (GFile *file)
{
  const char *path;
  static GQuark _file_path_quark = 0;

  if (G_UNLIKELY (_file_path_quark) == 0)
    _file_path_quark = g_quark_from_static_string ("gsystem-file-path");

  G_LOCK (pathname_cache);

  path = g_object_get_qdata ((GObject*)file, _file_path_quark);
  if (!path)
    {
      path = g_file_get_path (file);
      if (path == NULL)
        {
          G_UNLOCK (pathname_cache);
          return NULL;
        }
      g_object_set_qdata_full ((GObject*)file, _file_path_quark, (char*)path, (GDestroyNotify)g_free);
    }

  G_UNLOCK (pathname_cache);

  return path;
}

gboolean
rpmostree_str_has_prefix_in_ptrarray (const char *str,
                                      GPtrArray  *prefixes)
{
  for (guint j = 0; j < prefixes->len; j++)
    {
      const char *prefix = prefixes->pdata[j];
      if (g_str_has_prefix (str, prefix))
        return TRUE;
    }
  return FALSE;
}

/* Like g_strv_contains() but for ptrarray */
gboolean
rpmostree_str_ptrarray_contains (GPtrArray  *strs,
                                 const char *str)
{
  guint n = strs->len;
  for (guint i = 0; i < n; i++)
    {
      if (g_str_equal (str, strs->pdata[i]))
        return TRUE;
    }
  return FALSE;
}

gboolean
rpmostree_deployment_get_layered_info (OstreeRepo        *repo,
                                       OstreeDeployment  *deployment,
                                       gboolean          *out_is_layered,
                                       char             **out_base_layer,
                                       char            ***out_layered_pkgs,
                                       GVariant         **out_removed_base_pkgs,
                                       GVariant         **out_replaced_base_pkgs,
                                       GError           **error)
{
  const char *csum = ostree_deployment_get_csum (deployment);
  g_autoptr(GVariant) commit = NULL;
  if (!ostree_repo_load_commit (repo, csum, &commit, NULL, error))
    return FALSE;

  g_autoptr(GVariant) metadata = g_variant_get_child_value (commit, 0);
  g_autoptr(GVariantDict) dict = g_variant_dict_new (metadata);

  /* More recent versions have an explicit clientlayer attribute (which
   * realistically will always be TRUE). For older versions, we just
   * rely on the treespec being present. */
  gboolean is_layered = FALSE;
  if (!g_variant_dict_lookup (dict, "rpmostree.clientlayer", "b", &is_layered))
    is_layered = g_variant_dict_contains (dict, "rpmostree.spec");

  guint clientlayer_version = 0;
  g_variant_dict_lookup (dict, "rpmostree.clientlayer_version", "u",
                         &clientlayer_version);

  /* only fetch base if we have to */
  g_autofree char *base_layer = NULL;
  if (is_layered && out_base_layer != NULL)
    {
      base_layer = ostree_commit_get_parent (commit);
      g_assert (base_layer);
    }

  /* only fetch pkgs if we have to */
  g_auto(GStrv) layered_pkgs = NULL;
  g_autoptr(GVariant) removed_base_pkgs = NULL;
  g_autoptr(GVariant) replaced_base_pkgs = NULL;
  if (is_layered && (out_layered_pkgs != NULL || out_removed_base_pkgs != NULL))
    {
      /* starting from v1, we no longer embed a treespec in client layers */
      if (clientlayer_version >= 1)
        {
          g_assert (g_variant_dict_lookup (dict, "rpmostree.packages", "^as",
                                           &layered_pkgs));
        }
      else
        {
          g_autoptr(GVariant) treespec_v = NULL;
          g_autoptr(GVariantDict) treespec = NULL;

          g_assert (g_variant_dict_contains (dict, "rpmostree.spec"));

           /* there should always be a treespec */
          treespec_v = g_variant_dict_lookup_value (dict, "rpmostree.spec",
                                                    G_VARIANT_TYPE ("a{sv}"));
          g_assert (treespec_v);

          /* there should always be a packages entry, even if empty */
          treespec = g_variant_dict_new (treespec_v);
          g_assert (g_variant_dict_lookup (treespec, "packages", "^as",
                                           &layered_pkgs));
        }

      if (clientlayer_version >= 2)
        {
          removed_base_pkgs =
            g_variant_dict_lookup_value (dict, "rpmostree.removed-base-packages",
                                         G_VARIANT_TYPE ("av"));
          g_assert (removed_base_pkgs);

          replaced_base_pkgs =
            g_variant_dict_lookup_value (dict, "rpmostree.replaced-base-packages",
                                         G_VARIANT_TYPE ("a(vv)"));
          g_assert (replaced_base_pkgs);
        }
    }

  /* canonicalize outputs to empty array */

  if (out_is_layered != NULL)
    *out_is_layered = is_layered;
  if (out_base_layer != NULL)
    *out_base_layer = g_steal_pointer (&base_layer);
  if (out_layered_pkgs != NULL)
    {
      if (!layered_pkgs)
        layered_pkgs = g_new0 (char*, 1);
      *out_layered_pkgs = g_steal_pointer (&layered_pkgs);
    }
  if (out_removed_base_pkgs != NULL)
    {
      if (!removed_base_pkgs)
        removed_base_pkgs =
          g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("v"), NULL, 0));
      *out_removed_base_pkgs = g_steal_pointer (&removed_base_pkgs);
    }
  if (out_replaced_base_pkgs != NULL)
    {
      if (!replaced_base_pkgs)
        replaced_base_pkgs =
          g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(vv)"), NULL, 0));
      *out_replaced_base_pkgs = g_steal_pointer (&replaced_base_pkgs);
    }

  return TRUE;
}

gboolean
rpmostree_get_pkgcache_repo (OstreeRepo   *parent,
                             OstreeRepo  **out_pkgcache,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFile) pkgcache_path = NULL;

  /* get the GFile to it */
  {
    int parent_dfd = ostree_repo_get_dfd (parent); /* borrowed */
    g_autofree char *pkgcache_path_s =
      glnx_fdrel_abspath (parent_dfd, "extensions/rpmostree/pkgcache");
    pkgcache_path = g_file_new_for_path (pkgcache_path_s);
  }

  g_autoptr(OstreeRepo) pkgcache = ostree_repo_new (pkgcache_path);

  if (!g_file_query_exists (pkgcache_path, cancellable))
    {
      if (!g_file_make_directory_with_parents (pkgcache_path,
                                               cancellable, error))
        return FALSE;

      if (!ostree_repo_create (pkgcache, OSTREE_REPO_MODE_BARE,
                               cancellable, error))
        return FALSE;
    }

  if (!ostree_repo_open (pkgcache, cancellable, error))
    return FALSE;

  *out_pkgcache = g_steal_pointer (&pkgcache);

  return TRUE;
}

gboolean
rpmostree_decompose_sha256_nevra (const char **nevra, /* gets incremented */
                                  char       **out_sha256,
                                  GError     **error)
{
  const char *sha256_nevra = *nevra;
  g_autofree char *sha256 = NULL;

  if (strlen (sha256_nevra) < 66 || /* 64 + ":" + at least 1 char for nevra */
      sha256_nevra[64] != ':')
    return FALSE;

  sha256 = g_strndup (sha256_nevra, 64);
  if (!ostree_validate_checksum_string (sha256, error))
    return FALSE;

  *nevra += 65;
  if (out_sha256)
    *out_sha256 = g_steal_pointer (&sha256);

  return TRUE;
}

/* translates cachebranch back to nevra */
char *
rpmostree_cache_branch_to_nevra (const char *cachebranch)
{
  GString *r = g_string_new ("");
  const char *p;

  g_assert (g_str_has_prefix (cachebranch, "rpmostree/pkg/"));
  cachebranch += strlen ("rpmostree/pkg/");

  for (p = cachebranch; *p; p++)
    {
      char c = *p;

      if (c != '_')
        {
          if (c == '/')
            g_string_append_c (r, '-');
          else
            g_string_append_c (r, c);
          continue;
        }

      p++;
      c = *p;

      if (c == '_')
        {
          g_string_append_c (r, c);
          continue;
        }

      if (!*p || !*(p+1))
        break;

      const char h[3] = { *p, *(p+1) };
      g_string_append_c (r, g_ascii_strtoull (h, NULL, 16));
      p++;
    }

  return g_string_free (r, FALSE);
}

/* Given the result of rpm_ostree_db_diff(), print it. */
void
rpmostree_diff_print (OstreeRepo *repo,
                      GPtrArray *removed,
                      GPtrArray *added,
                      GPtrArray *modified_old,
                      GPtrArray *modified_new)
{
  gboolean first;

  g_assert (modified_old->len == modified_new->len);

  first = TRUE;
  for (guint i = 0; i < modified_old->len; i++)
    {
      RpmOstreePackage *oldpkg = modified_old->pdata[i];
      RpmOstreePackage *newpkg = modified_new->pdata[i];
      const char *name = rpm_ostree_package_get_name (oldpkg);

      if (rpm_ostree_package_cmp (oldpkg, newpkg) > 0)
        continue;

      if (first)
        {
          g_print ("Upgraded:\n");
          first = FALSE;
        }

      g_print ("  %s %s -> %s\n", name,
               rpm_ostree_package_get_evr (oldpkg),
               rpm_ostree_package_get_evr (newpkg));
    }

  first = TRUE;
  for (guint i = 0; i < modified_old->len; i++)
    {
      RpmOstreePackage *oldpkg = modified_old->pdata[i];
      RpmOstreePackage *newpkg = modified_new->pdata[i];
      const char *name = rpm_ostree_package_get_name (oldpkg);

      if (rpm_ostree_package_cmp (oldpkg, newpkg) < 0)
        continue;

      if (first)
        {
          g_print ("Downgraded:\n");
          first = FALSE;
        }

      g_print ("  %s %s -> %s\n", name,
               rpm_ostree_package_get_evr (oldpkg),
               rpm_ostree_package_get_evr (newpkg));
    }

  if (removed->len > 0)
    g_print ("Removed:\n");
  for (guint i = 0; i < removed->len; i++)
    {
      RpmOstreePackage *pkg = removed->pdata[i];
      const char *nevra = rpm_ostree_package_get_nevra (pkg);

      g_print ("  %s\n", nevra);
    }

  if (added->len > 0)
    g_print ("Added:\n");
  for (guint i = 0; i < added->len; i++)
    {
      RpmOstreePackage *pkg = added->pdata[i];
      const char *nevra = rpm_ostree_package_get_nevra (pkg);

      g_print ("  %s\n", nevra);
    }
}
