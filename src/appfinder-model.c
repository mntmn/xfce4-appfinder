/*
 * Copyright (C) 2011 Nick Schermer <nick@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>

#include <src/appfinder-model.h>
#include <src/appfinder-private.h>



#define HISTORY_PATH "xfce4/xfce4-appfinder/history"



static void               xfce_appfinder_model_tree_model_init       (GtkTreeModelIface        *iface);
static void               xfce_appfinder_model_finalize              (GObject                  *object);
static GtkTreeModelFlags  xfce_appfinder_model_get_flags             (GtkTreeModel             *tree_model);
static gint               xfce_appfinder_model_get_n_columns         (GtkTreeModel             *tree_model);
static GType              xfce_appfinder_model_get_column_type       (GtkTreeModel             *tree_model,
                                                                      gint                      column);
static gboolean           xfce_appfinder_model_get_iter              (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter,
                                                                      GtkTreePath              *path);
static GtkTreePath       *xfce_appfinder_model_get_path              (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter);
static void               xfce_appfinder_model_get_value             (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter,
                                                                      gint                      column,
                                                                      GValue                   *value);
static gboolean           xfce_appfinder_model_iter_next             (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter);
static gboolean           xfce_appfinder_model_iter_children         (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter,
                                                                      GtkTreeIter              *parent);
static gboolean           xfce_appfinder_model_iter_has_child        (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter);
static gint               xfce_appfinder_model_iter_n_children       (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter);
static gboolean           xfce_appfinder_model_iter_nth_child        (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter,
                                                                      GtkTreeIter              *parent,
                                                                      gint                      n);
static gboolean           xfce_appfinder_model_iter_parent           (GtkTreeModel             *tree_model,
                                                                      GtkTreeIter              *iter,
                                                                      GtkTreeIter              *child);
static void               xfce_appfinder_model_menu_changed          (GarconMenu               *menu,
                                                                      XfceAppfinderModel       *model);
static gpointer           xfce_appfinder_model_collect_thread        (gpointer                  user_data);
static void               xfce_appfinder_model_item_free             (gpointer                  data,
                                                                      XfceAppfinderModel       *model);



struct _XfceAppfinderModelClass
{
  GObjectClass __parent__;
};

struct _XfceAppfinderModel
{
  GObject              __parent__;
  gint                 stamp;

  GSList              *items;
  GHashTable          *items_hash;

  GarconMenu          *menu;

  GdkPixbuf           *command_icon_small;
  GdkPixbuf           *command_icon_large;
  GarconMenuDirectory *command_category;

  GSList              *categories;

  guint                collect_idle_id;
  GSList              *collect_items;
  GSList              *collect_categories;
  GThread             *collect_thread;
  volatile gboolean    collect_cancelled;
  GHashTable          *collect_desktop_ids;
};

typedef struct
{
  GarconMenuItem *item;
  gchar          *key;
  gchar          *abstract;
  GPtrArray      *categories;
  gchar          *command;
  gchar          *tooltip;

  GdkPixbuf      *icon_small;
  GdkPixbuf      *icon_large;
}
ModelItem;

enum
{
  CATEGORIES_CHANGED,
  LAST_SIGNAL
};



static guint model_signals[LAST_SIGNAL];



G_DEFINE_TYPE_WITH_CODE (XfceAppfinderModel, xfce_appfinder_model, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GTK_TYPE_TREE_MODEL, xfce_appfinder_model_tree_model_init))



static void
xfce_appfinder_model_class_init (XfceAppfinderModelClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = xfce_appfinder_model_finalize;

  model_signals[CATEGORIES_CHANGED] =
    g_signal_new (g_intern_static_string ("categories-changed"),
                  G_TYPE_FROM_CLASS (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE, 1,
                  G_TYPE_POINTER /* GSList */);
}



static void
xfce_appfinder_model_init (XfceAppfinderModel *model)
{
  /* generate a unique stamp */
  model->stamp = g_random_int ();
  model->items_hash = g_hash_table_new (g_str_hash, g_str_equal);
  model->command_icon_small = xfce_appfinder_model_load_pixbuf (GTK_STOCK_EXECUTE, ICON_SMALL);
  model->command_icon_large = xfce_appfinder_model_load_pixbuf (GTK_STOCK_EXECUTE, ICON_LARGE);
  model->command_category = xfce_appfinder_model_get_command_category ();

  model->menu = garcon_menu_new_applications ();
  model->collect_thread = g_thread_create (xfce_appfinder_model_collect_thread, model, TRUE, NULL);
}



static void
xfce_appfinder_model_tree_model_init (GtkTreeModelIface *iface)
{
  iface->get_flags = xfce_appfinder_model_get_flags;
  iface->get_n_columns = xfce_appfinder_model_get_n_columns;
  iface->get_column_type = xfce_appfinder_model_get_column_type;
  iface->get_iter = xfce_appfinder_model_get_iter;
  iface->get_path = xfce_appfinder_model_get_path;
  iface->get_value = xfce_appfinder_model_get_value;
  iface->iter_next = xfce_appfinder_model_iter_next;
  iface->iter_children = xfce_appfinder_model_iter_children;
  iface->iter_has_child = xfce_appfinder_model_iter_has_child;
  iface->iter_n_children = xfce_appfinder_model_iter_n_children;
  iface->iter_nth_child = xfce_appfinder_model_iter_nth_child;
  iface->iter_parent = xfce_appfinder_model_iter_parent;
}



static void
xfce_appfinder_model_finalize (GObject *object)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (object);

  /* join the collector thread */
  model->collect_cancelled = TRUE;
  g_thread_join (model->collect_thread);

  /* cancel any pending collect idle source */
  if (G_UNLIKELY (model->collect_idle_id != 0))
    g_source_remove (model->collect_idle_id);
  g_slist_free (model->collect_categories);

  g_signal_handlers_disconnect_by_func (G_OBJECT (model->menu),
      G_CALLBACK (xfce_appfinder_model_menu_changed), model);
  g_object_unref (G_OBJECT (model->menu));

  g_slist_foreach (model->collect_items, (GFunc) xfce_appfinder_model_item_free, model);
  g_slist_free (model->collect_items);
  g_slist_foreach (model->items, (GFunc) xfce_appfinder_model_item_free, model);
  g_slist_free (model->items);

  g_slist_foreach (model->collect_categories, (GFunc) xfce_appfinder_model_category_free, NULL);
  g_slist_free (model->collect_categories);
  g_slist_foreach (model->categories, (GFunc) xfce_appfinder_model_category_free, NULL);
  g_slist_free (model->categories);

  if (model->collect_desktop_ids != NULL)
    g_hash_table_destroy (model->collect_desktop_ids);
  g_hash_table_destroy (model->items_hash);

  g_object_unref (G_OBJECT (model->command_icon_large));
  g_object_unref (G_OBJECT (model->command_icon_small));
  g_object_unref (G_OBJECT (model->command_category));

  APPFINDER_DEBUG ("model cleared");

  (*G_OBJECT_CLASS (xfce_appfinder_model_parent_class)->finalize) (object);
}



static GtkTreeModelFlags
xfce_appfinder_model_get_flags (GtkTreeModel *tree_model)
{
  return GTK_TREE_MODEL_ITERS_PERSIST | GTK_TREE_MODEL_LIST_ONLY;
}



static gint
xfce_appfinder_model_get_n_columns (GtkTreeModel *tree_model)
{
  return XFCE_APPFINDER_MODEL_N_COLUMNS;
}



static GType
xfce_appfinder_model_get_column_type (GtkTreeModel *tree_model,
                                      gint          column)
{
  switch (column)
    {
    case XFCE_APPFINDER_MODEL_COLUMN_ABSTRACT:
    case XFCE_APPFINDER_MODEL_COLUMN_URI:
    case XFCE_APPFINDER_MODEL_COLUMN_COMMAND:
    case XFCE_APPFINDER_MODEL_COLUMN_TOOLTIP:
      return G_TYPE_STRING;

    case XFCE_APPFINDER_MODEL_COLUMN_ICON_SMALL:
    case XFCE_APPFINDER_MODEL_COLUMN_ICON_LARGE:
      return GDK_TYPE_PIXBUF;

    default:
      g_assert_not_reached ();
      return G_TYPE_INVALID;
    }
}



static gboolean
xfce_appfinder_model_get_iter (GtkTreeModel *tree_model,
                               GtkTreeIter  *iter,
                               GtkTreePath  *path)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (tree_model);

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);
  g_return_val_if_fail (gtk_tree_path_get_depth (path) > 0, FALSE);

  iter->stamp = model->stamp;
  iter->user_data = g_slist_nth (model->items, gtk_tree_path_get_indices (path)[0]);

  return (iter->user_data != NULL);
}



static GtkTreePath*
xfce_appfinder_model_get_path (GtkTreeModel *tree_model,
                               GtkTreeIter  *iter)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (tree_model);
  gint                idx;

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), NULL);
  g_return_val_if_fail (iter->stamp == model->stamp, NULL);

  /* determine the index of the iter */
  idx = g_slist_position (model->items, iter->user_data);
  if (G_UNLIKELY (idx < 0))
    return NULL;

  return gtk_tree_path_new_from_indices (idx, -1);
}



static void
xfce_appfinder_model_get_value (GtkTreeModel *tree_model,
                                GtkTreeIter  *iter,
                                gint          column,
                                GValue       *value)
{
  XfceAppfinderModel  *model = XFCE_APPFINDER_MODEL (tree_model);
  ModelItem           *item;
  const gchar         *name;
  const gchar         *comment;
  GFile               *file;
  gchar               *parse_name;
  GList               *categories, *li;
  gchar              **cat_arr;
  gchar               *cat_str;
  guint                i;

  g_return_if_fail (XFCE_IS_APPFINDER_MODEL (model));
  g_return_if_fail (iter->stamp == model->stamp);

  item = ITER_GET_DATA (iter);
  g_return_if_fail ((item->item == NULL && item->command != NULL)
                    || (item->item != NULL && GARCON_IS_MENU_ITEM (item->item)));

  switch (column)
    {
    case XFCE_APPFINDER_MODEL_COLUMN_ABSTRACT:
      if (item->abstract == NULL)
        {
          if (item->item != NULL)
            {
              name = garcon_menu_item_get_name (item->item);
              comment = garcon_menu_item_get_comment (item->item);

              if (comment != NULL)
                item->abstract = g_markup_printf_escaped ("<b>%s</b>\n%s", name, comment);
              else
                item->abstract = g_markup_printf_escaped ("<b>%s</b>", name);
            }
          else if (item->command != NULL)
            {
              item->abstract = g_markup_escape_text (item->command, -1);
            }
        }

      g_value_init (value, G_TYPE_STRING);
      g_value_set_static_string (value, item->abstract);
      break;

    case XFCE_APPFINDER_MODEL_COLUMN_COMMAND:
      g_value_init (value, G_TYPE_STRING);
      g_value_set_static_string (value, item->command);
      break;

    case XFCE_APPFINDER_MODEL_COLUMN_ICON_SMALL:
      if (item->icon_small == NULL
          && item->item != NULL)
        {
          name = garcon_menu_item_get_icon_name (item->item);
          item->icon_small = xfce_appfinder_model_load_pixbuf (name, ICON_SMALL);
        }

      g_value_init (value, GDK_TYPE_PIXBUF);
      g_value_set_object (value, item->icon_small);
      break;

    case XFCE_APPFINDER_MODEL_COLUMN_ICON_LARGE:
      if (item->icon_large == NULL
          && item->item != NULL)
        {
          name = garcon_menu_item_get_icon_name (item->item);
          item->icon_large = xfce_appfinder_model_load_pixbuf (name, ICON_LARGE);
        }

      g_value_init (value, GDK_TYPE_PIXBUF);
      g_value_set_object (value, item->icon_large);
      break;

    case XFCE_APPFINDER_MODEL_COLUMN_TOOLTIP:
      if (item->item != NULL
          && item->tooltip == NULL)
        {
          file = garcon_menu_item_get_file (item->item);
          parse_name = g_file_get_parse_name (file);
          g_object_unref (G_OBJECT (file));

          /* create nice category string */
          categories = garcon_menu_item_get_categories (item->item);
          i = g_list_length (categories);
          cat_arr = g_new0 (gchar *, i + 1);
          for (li = categories; li != NULL; li = li->next)
            cat_arr[--i] = li->data;
          cat_str = g_strjoinv ("; ", cat_arr);
          g_free (cat_arr);

          item->tooltip = g_markup_printf_escaped ("<b>%s:</b> %s\n"
                                                   "<b>%s:</b> %s\n"
                                                   "<b>%s:</b> %s\n"
                                                   "<b>%s:</b> %s",
                                                   _("Name"), garcon_menu_item_get_name (item->item),
                                                   _("Command"), garcon_menu_item_get_command (item->item),
                                                   _("Categories"), cat_str,
                                                   _("Filename"), parse_name);

          g_free (parse_name);
          g_free (cat_str);
        }

      g_value_init (value, G_TYPE_STRING);
      g_value_set_string (value, item->tooltip);
      break;

    case XFCE_APPFINDER_MODEL_COLUMN_URI:
      g_value_init (value, G_TYPE_STRING);
      if (item->item != NULL)
        g_value_take_string (value, garcon_menu_item_get_uri (item->item));
      break;

    default:
      g_assert_not_reached ();
      break;
    }
}



static gboolean
xfce_appfinder_model_iter_next (GtkTreeModel *tree_model,
                                GtkTreeIter  *iter)
{
  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (tree_model), FALSE);
  g_return_val_if_fail (iter->stamp == XFCE_APPFINDER_MODEL (tree_model)->stamp, FALSE);

  iter->user_data = g_slist_next (iter->user_data);
  return (iter->user_data != NULL);
}



static gboolean
xfce_appfinder_model_iter_children (GtkTreeModel *tree_model,
                                    GtkTreeIter  *iter,
                                    GtkTreeIter  *parent)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (tree_model);

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);

  if (G_LIKELY (parent == NULL && model->items != NULL))
    {
      iter->stamp = model->stamp;
      iter->user_data = model->items;
      return TRUE;
    }

  return FALSE;
}



static gboolean
xfce_appfinder_model_iter_has_child (GtkTreeModel *tree_model,
                                     GtkTreeIter  *iter)
{
  return FALSE;
}



static gint
xfce_appfinder_model_iter_n_children (GtkTreeModel *tree_model,
                                      GtkTreeIter  *iter)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (tree_model);

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), 0);

  return (iter == NULL) ? g_slist_length (model->items) : 0;
}



static gboolean
xfce_appfinder_model_iter_nth_child (GtkTreeModel *tree_model,
                                     GtkTreeIter  *iter,
                                     GtkTreeIter  *parent,
                                     gint          n)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (tree_model);

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);

  if (G_LIKELY (parent != NULL))
    {
      iter->stamp = model->stamp;
      iter->user_data = g_slist_nth (model->items, n);
      return (iter->user_data != NULL);
    }

  return FALSE;
}



static gboolean
xfce_appfinder_model_iter_parent (GtkTreeModel *tree_model,
                                  GtkTreeIter  *iter,
                                  GtkTreeIter  *child)
{
  return FALSE;
}



static gboolean
xfce_appfinder_model_collect_idle (gpointer user_data)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (user_data);
  GtkTreePath        *path;
  GtkTreeIter         iter;
  GSList             *li, *lnext;
  GSList             *tmp;

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);
  g_return_val_if_fail (model->items == NULL, FALSE);

  APPFINDER_DEBUG ("insert idle start");

  GDK_THREADS_ENTER ();

  /* move the collected items "online" */
  model->items = model->collect_items;
  model->collect_items = NULL;

  /* emit notifications for all new items */
  path = gtk_tree_path_new_first ();
  for (li = model->items; li != NULL; li = li->next)
    {
      /* remember the next item */
      lnext = li->next;
      li->next = NULL;

      /* generate the iterator */
      ITER_INIT (iter, model->stamp, li);

      /* emit the "row-inserted" signal */
      gtk_tree_model_row_inserted (GTK_TREE_MODEL (model), path, &iter);

      /* advance the path */
      gtk_tree_path_next (path);

      /* reset the next item */
      li->next = lnext;
    }
  gtk_tree_path_free (path);

  /* signal new categories */
  if (model->collect_categories != NULL)
    {
      tmp = model->categories;
      model->categories = model->collect_categories;
      model->collect_categories = NULL;

      g_signal_emit (G_OBJECT (model), model_signals[CATEGORIES_CHANGED], 0, model->categories);

      g_slist_foreach (tmp, (GFunc) xfce_appfinder_model_category_free, NULL);
      g_slist_free (tmp);
    }

  GDK_THREADS_LEAVE ();

  APPFINDER_DEBUG ("insert idle end");

  return FALSE;
}



static void
xfce_appfinder_model_collect_idle_destroy (gpointer user_data)
{
  XFCE_APPFINDER_MODEL (user_data)->collect_idle_id = 0;
}



static gint
xfce_appfinder_model_item_compare (gconstpointer a,
                                   gconstpointer b)
{
  const ModelItem *item_a = a, *item_b = b;
  const gchar     *name_a, *name_b;

  /* sort custom commands before desktop files */
  if ((item_a->item != NULL) != (item_b->item != NULL))
    return (item_a->item != NULL) ? 1 : -1;

  /* sort desktop entries */
  if (item_a->item != NULL)
    {
      name_a = garcon_menu_item_get_name (item_a->item);
      name_b = garcon_menu_item_get_name (item_b->item);
      return g_utf8_collate (name_a, name_b);
    }

  /* sort custom commands */
  return g_utf8_collate (item_a->command, item_b->command);
}



static gchar *
xfce_appfinder_model_item_key (GarconMenuItem *item)
{
  const gchar *value;
  GString     *str;
  gchar       *normalized;
  gchar       *casefold;
  gchar       *p;

  str = g_string_sized_new (128);

  value = garcon_menu_item_get_name (item);
  if (value != NULL)
    g_string_append (str, value);
  g_string_append_c (str, '\n');

  value = garcon_menu_item_get_command (item);
  if (value != NULL)
    {
      /* only add first part of the command */
      p = strchr (value, ' ');
      g_string_append_len (str, value, p != NULL ? p - value : -1);
    }
  g_string_append_c (str, '\n');

  value = garcon_menu_item_get_comment (item);
  if (value != NULL)
    g_string_append (str, value);

  normalized = g_utf8_normalize (str->str, str->len, G_NORMALIZE_ALL);
  casefold = g_utf8_casefold (normalized, -1);
  g_free (normalized);

  g_string_free (str, TRUE);

  return casefold;
}



static ModelItem *
xfce_appfinder_model_item_new (GarconMenuItem *menu_item)
{
  ModelItem   *item;
  const gchar *command, *p;

  g_return_val_if_fail (GARCON_IS_MENU_ITEM (menu_item), NULL);

  item = g_slice_new0 (ModelItem);
  item->item = g_object_ref (G_OBJECT (menu_item));

  command = garcon_menu_item_get_command (menu_item);
  if (G_LIKELY (command != NULL))
    {
      p = strchr (command, ' ');
      if (p != NULL)
        item->command = g_strndup (command, p - command);
      else
        item->command = g_strdup (command);
    }

  item->key = xfce_appfinder_model_item_key (menu_item);

  return item;
}



static void
xfce_appfinder_model_item_changed (GarconMenuItem     *menu_item,
                                   XfceAppfinderModel *model)
{
  GSList      *li;
  ModelItem   *item;
  gint         idx;
  GtkTreeIter  iter;
  GtkTreePath *path;
  GPtrArray   *categories;

  /* lookup the item in the list */
  for (li = model->items, idx = 0; li != NULL; li = li->next, idx++)
    {
      item = li->data;
      if (item->item == menu_item)
        {
          categories = g_ptr_array_ref (item->categories);
          g_object_ref (G_OBJECT (menu_item));

          APPFINDER_DEBUG ("update item %s", garcon_menu_item_get_desktop_id (menu_item));

          g_hash_table_remove (model->items_hash, item->command);
          xfce_appfinder_model_item_free (item, model);

          item = xfce_appfinder_model_item_new (menu_item);
          item->categories = categories;
          li->data = item;

          if (G_LIKELY (item->command != NULL))
            g_hash_table_insert (model->items_hash, item->command, item);

          g_object_unref (G_OBJECT (menu_item));

          path = gtk_tree_path_new_from_indices (idx, -1);
          ITER_INIT (iter, model->stamp, li);
          gtk_tree_model_row_changed (GTK_TREE_MODEL (model), path, &iter);
          gtk_tree_path_free (path);

          break;
        }
    }
}



static void
xfce_appfinder_model_item_free (gpointer            data,
                                XfceAppfinderModel *model)
{
  ModelItem *item = data;

  if (item->item != NULL)
    {
      g_signal_handlers_disconnect_by_func (G_OBJECT (item->item),
          G_CALLBACK (xfce_appfinder_model_item_changed), model);
      g_object_unref (G_OBJECT (item->item));
    }
  if (item->icon_small != NULL)
    g_object_unref (G_OBJECT (item->icon_small));
  if (item->icon_large != NULL)
    g_object_unref (G_OBJECT (item->icon_large));
  if (item->categories != NULL)
    g_ptr_array_unref (item->categories);
  g_free (item->abstract);
  g_free (item->key);
  g_free (item->command);
  g_free (item->tooltip);
  g_slice_free (ModelItem, item);
}




static gint
xfce_appfinder_model_category_compare (gconstpointer a,
                                       gconstpointer b)
{
  const CategoryItem *cat_a = a;
  const CategoryItem *cat_b = b;

  g_return_val_if_fail (GARCON_IS_MENU_DIRECTORY (cat_a->directory), 0);
  g_return_val_if_fail (GARCON_IS_MENU_DIRECTORY (cat_b->directory), 0);

  return g_utf8_collate (garcon_menu_directory_get_name (cat_a->directory),
                         garcon_menu_directory_get_name (cat_b->directory));
}



void
xfce_appfinder_model_category_free (CategoryItem *item)
{
  if (item->directory != NULL)
    g_object_unref (G_OBJECT (item->directory));
  if (item->pixbuf != NULL)
    g_object_unref (G_OBJECT (item->pixbuf));
  g_slice_free (CategoryItem, item);
}



static gboolean
xfce_appfinder_model_ptr_array_find (GPtrArray     *array,
                                     gconstpointer  data)
{
  guint i;

  if (array != NULL && data != NULL)
    for (i = 0; i < array->len; i++)
      if (g_ptr_array_index (array, i) == data)
        return TRUE;

  return FALSE;
}



static gboolean
xfce_appfinder_model_collect_items (XfceAppfinderModel  *model,
                                    GarconMenu          *menu,
                                    GarconMenuDirectory *category)
{
  GList               *elements, *li;
  GarconMenuDirectory *directory;
  ModelItem           *item;
  gboolean             has_items = FALSE;
  const gchar         *desktop_id;
  CategoryItem        *citem;

  g_return_val_if_fail (GARCON_IS_MENU (menu), FALSE);
  g_return_val_if_fail (category == NULL || GARCON_IS_MENU_DIRECTORY (category), FALSE);

  directory = garcon_menu_get_directory (menu);
  if (directory != NULL)
    {
      if (!garcon_menu_directory_get_visible (directory))
        return FALSE;

      /* this way we only have two levels */
      if (category == NULL)
        category = directory;
    }

  /* collect all the elements in this menu and its sub menus */
  elements = garcon_menu_get_elements (menu);
  for (li = elements; li != NULL && !model->collect_cancelled; li = li->next)
    {
      if (GARCON_IS_MENU_ITEM (li->data))
        {
          if (!garcon_menu_element_get_visible (li->data))
            continue;

          desktop_id = garcon_menu_item_get_desktop_id (li->data);
          item = g_hash_table_lookup (model->collect_desktop_ids, desktop_id);
          if (G_LIKELY (item == NULL))
            {
              item = xfce_appfinder_model_item_new (li->data);

              item->categories = g_ptr_array_new_with_free_func (g_object_unref);
              if (category != NULL)
                g_ptr_array_add (item->categories, g_object_ref (G_OBJECT (category)));

              model->collect_items = g_slist_prepend (model->collect_items, item);
              g_hash_table_insert (model->collect_desktop_ids, (gchar *) desktop_id, item);
              if (G_LIKELY (item->command != NULL))
                g_hash_table_insert (model->items_hash, item->command, item);

              g_signal_connect (G_OBJECT (item->item), "changed",
                  G_CALLBACK (xfce_appfinder_model_item_changed), model);
            }
          else if (category != NULL
                   && !xfce_appfinder_model_ptr_array_find (item->categories, category))
            {
              /* add category to existing item */
              g_ptr_array_add (item->categories, g_object_ref (G_OBJECT (category)));
              APPFINDER_DEBUG ("%s is in %d categories", desktop_id, item->categories->len);
            }

          has_items = TRUE;
        }
      else if (GARCON_IS_MENU (li->data))
        {
          if (xfce_appfinder_model_collect_items (model, li->data, category))
            has_items = TRUE;
        }
    }
  g_list_free (elements);

  if (directory != NULL && has_items)
    {
      citem = g_slice_new0 (CategoryItem);
      citem->directory = g_object_ref (G_OBJECT (directory));
      model->collect_categories = g_slist_prepend (model->collect_categories, citem);
    }

  return has_items;
}



static void
xfce_appfinder_model_collect_history (XfceAppfinderModel *model,
                                      GMappedFile        *history)
{
  gchar     *contents, *end;
  gsize      len;
  ModelItem *item;

  contents = g_mapped_file_get_contents (history);
  if (contents == NULL)
    return;

  for (;!model->collect_cancelled;)
    {
      end = strchr (contents, '\n');
      if (G_UNLIKELY (end == NULL))
        break;

      len = end - contents;
      if (len > 0)
        {
          item = g_slice_new0 (ModelItem);
          item->command = g_strndup (contents, len);
          item->icon_small = g_object_ref (G_OBJECT (model->command_icon_small));
          item->icon_large = g_object_ref (G_OBJECT (model->command_icon_large));
          model->collect_items = g_slist_prepend (model->collect_items, item);

          g_hash_table_insert (model->items_hash, item->command, item);
        }

      contents += len + 1;
    }
}



static void
xfce_appfinder_model_menu_changed (GarconMenu         *menu,
                                   XfceAppfinderModel *model)
{
  g_return_if_fail (GARCON_IS_MENU (menu));
  g_return_if_fail (XFCE_IS_APPFINDER_MODEL (model));

  /* TODO */
}



static gpointer
xfce_appfinder_model_collect_thread (gpointer user_data)
{
  XfceAppfinderModel *model = XFCE_APPFINDER_MODEL (user_data);
  GError             *error = NULL;
  gchar              *filename;
  GMappedFile        *history;

  g_return_val_if_fail (GARCON_IS_MENU (model->menu), NULL);
  g_return_val_if_fail (model->collect_items == NULL, NULL);
  g_return_val_if_fail (model->collect_desktop_ids == NULL, NULL);

  APPFINDER_DEBUG ("collect thread start");

  /* load menu data */
  if (G_LIKELY (model->menu != NULL))
    {
      if (garcon_menu_load (model->menu, NULL, &error))
        {
          model->collect_desktop_ids = g_hash_table_new (g_str_hash, g_str_equal);
          xfce_appfinder_model_collect_items (model, model->menu, NULL);
          g_hash_table_destroy (model->collect_desktop_ids);
          model->collect_desktop_ids = NULL;
        }
      else
        {
          g_warning ("Failed to load the root menu: %s", error->message);
          g_clear_error (&error);
        }
    }

  /* load command history */
  filename = xfce_resource_lookup (XFCE_RESOURCE_CACHE, HISTORY_PATH);
  if (G_LIKELY (filename != NULL))
    {
      history = g_mapped_file_new (filename, FALSE, &error);
      if (G_LIKELY (history != NULL))
        {
          xfce_appfinder_model_collect_history (model, history);
          g_mapped_file_free (history);
        }
      else
        {
          g_warning ("Failed to open history file: %s", error->message);
          g_error_free (error);
        }
    }

  if (model->collect_items != NULL
      && !model->collect_cancelled)
    {
      model->collect_items = g_slist_sort (model->collect_items, xfce_appfinder_model_item_compare);
      model->collect_categories = g_slist_sort (model->collect_categories, xfce_appfinder_model_category_compare);

      model->collect_idle_id = g_idle_add_full (G_PRIORITY_LOW, xfce_appfinder_model_collect_idle,
                                                model, xfce_appfinder_model_collect_idle_destroy);
    }

  if (G_LIKELY (model->menu != NULL))
    {
      g_signal_connect (G_OBJECT (model->menu), "reload-required",
                        G_CALLBACK (xfce_appfinder_model_menu_changed), model);
    }

  APPFINDER_DEBUG ("collect thread end");

  return NULL;
}



XfceAppfinderModel *
xfce_appfinder_model_get (void)
{
  static XfceAppfinderModel *model = NULL;

  if (G_LIKELY (model != NULL))
    {
      g_object_ref (G_OBJECT (model));
    }
  else
    {
      model = g_object_new (XFCE_TYPE_APPFINDER_MODEL, NULL);
      g_object_add_weak_pointer (G_OBJECT (model), (gpointer) &model);
      APPFINDER_DEBUG ("allocate new model");
    }

  return model;
}



GSList *
xfce_appfinder_model_get_categories (XfceAppfinderModel *model)
{
  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), NULL);
  return model->categories;
}



gboolean
xfce_appfinder_model_get_visible (XfceAppfinderModel        *model,
                                  const GtkTreeIter         *iter,
                                  const GarconMenuDirectory *category,
                                  const gchar               *string)
{
  ModelItem *item;

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);
  g_return_val_if_fail (iter->stamp == model->stamp, FALSE);
  g_return_val_if_fail (category == NULL || GARCON_IS_MENU_DIRECTORY (category), FALSE);
  g_return_val_if_fail (GARCON_IS_MENU_DIRECTORY (model->command_category), FALSE);

  item = ITER_GET_DATA (iter);

  if (item->item != NULL)
    {
      g_return_val_if_fail (GARCON_IS_MENU_ITEM (item->item), FALSE);

      if (category != NULL
          && !xfce_appfinder_model_ptr_array_find (item->categories, category))
        return FALSE;

      if (string != NULL
          && item->key != NULL)
        return strstr (item->key, string) != NULL;
    }
  else /* command item */
    {
      g_return_val_if_fail (item->command != NULL, FALSE);

      if (category != model->command_category)
        return FALSE;

      if (string != NULL)
        return strstr (item->command, string) != NULL;
    }

  return TRUE;
}



gboolean
xfce_appfinder_model_execute (XfceAppfinderModel  *model,
                              const GtkTreeIter   *iter,
                              GdkScreen           *screen,
                              gboolean            *is_regular_command,
                              GError             **error)
{
  const gchar     *command, *p;
  GarconMenuItem  *item;
  ModelItem       *mitem;
  GString         *string;
  gboolean         succeed = FALSE;
  gchar          **argv;

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);
  g_return_val_if_fail (iter->stamp == model->stamp, FALSE);
  g_return_val_if_fail (GDK_IS_SCREEN (screen), FALSE);

  mitem = ITER_GET_DATA (iter);
  item = mitem->item;

  /* leave if this is not a menu item */
  *is_regular_command = (item == NULL);
  if (item == NULL)
    return FALSE;

  g_return_val_if_fail (GARCON_IS_MENU_ITEM (item), FALSE);

  command = garcon_menu_item_get_command (item);
  if (!IS_STRING (command))
    {
      g_set_error_literal (error, 0, 0, _("Application has no command"));
      return FALSE;
    }

  string = g_string_sized_new (100);

  if (garcon_menu_item_requires_terminal (item))
    g_string_append (string, "exo-open --launch TerminalEmulator ");

  /* expand the field codes */
  for (p = command; *p != '\0'; ++p)
    {
      if (G_UNLIKELY (p[0] == '%' && p[1] != '\0'))
        {
          switch (*++p)
            {
            case '%':
              g_string_append_c (string, '%');
              break;

            /* skip all the other %? values for now we don't have dnd anyways */
            }
        }
      else
        {
          g_string_append_c (string, *p);
        }
    }

  if (g_shell_parse_argv (string->str, NULL, &argv, error))
    {
      succeed = xfce_spawn_on_screen (screen, garcon_menu_item_get_path (item),
                                      argv, NULL, G_SPAWN_SEARCH_PATH,
                                      garcon_menu_item_supports_startup_notification (item),
                                      gtk_get_current_event_time (),
                                      garcon_menu_item_get_icon_name (item),
                                      error);

      g_strfreev (argv);
    }

  g_string_free (string, TRUE);

  return succeed;
}



GdkPixbuf *
xfce_appfinder_model_load_pixbuf (const gchar *icon_name,
                                  gint         size)
{
  GdkPixbuf *pixbuf = NULL;
  GdkPixbuf *scaled;

  APPFINDER_DEBUG ("load icon %s at %dpx", icon_name, size);

  if (icon_name != NULL)
    {
      if (g_path_is_absolute (icon_name))
        {
          pixbuf = gdk_pixbuf_new_from_file_at_scale (icon_name, size,
                                                      size, TRUE, NULL);
        }
      else
        {
          pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
                                             icon_name, size, 0, NULL);
        }
    }

  if (G_UNLIKELY (pixbuf == NULL))
    {
      pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
                                         "applications-other",
                                         size, 0, NULL);
    }

  if (pixbuf != NULL
      && (gdk_pixbuf_get_width (pixbuf) > size
          || gdk_pixbuf_get_height (pixbuf) > size))
    {
      scaled = gdk_pixbuf_scale_simple (pixbuf, size, size, GDK_INTERP_BILINEAR);
      g_object_unref (G_OBJECT (pixbuf));
      pixbuf = scaled;
    }

  return pixbuf;
}



gboolean
xfce_appfinder_model_save_command (XfceAppfinderModel  *model,
                                   const gchar         *command,
                                   GError             **error)
{
  GSList    *li;
  GString   *contents;
  gboolean   succeed = FALSE;
  gchar     *filename;
  ModelItem *item;

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), FALSE);

  if (!IS_STRING (command)
      || g_hash_table_lookup (model->items_hash, command) != NULL)
    return TRUE;

  contents = g_string_new (NULL);

  APPFINDER_DEBUG ("saving history");

  /* store all the custom commands */
  for (li = model->items; li != NULL; li = li->next)
    {
      item = li->data;
      if (item->item != NULL
          || item->command == NULL)
        continue;

      g_string_append (contents, item->command);
      g_string_append_c (contents, '\n');
    }

  /* add the new command */
  g_string_append (contents, command);
  g_string_append_c (contents, '\n');

  if (contents->len > 0)
    {
      filename = xfce_resource_save_location (XFCE_RESOURCE_CACHE, HISTORY_PATH, TRUE);
      if (G_LIKELY (filename != NULL))
        succeed = g_file_set_contents (filename, contents->str, contents->len, error);
      else
        g_set_error_literal (error, 0, 0, "Unable to create history cache file");
      g_free (filename);
    }
  else
    {
      succeed = TRUE;
    }

  g_string_free (contents, TRUE);

  return succeed;
}



GdkPixbuf *
xfce_appfinder_model_get_icon_for_command (XfceAppfinderModel *model,
                                           const gchar        *command)
{
  ModelItem   *item;
  const gchar *icon_name;

  g_return_val_if_fail (XFCE_IS_APPFINDER_MODEL (model), NULL);

  if (IS_STRING (command))
    {
      item = g_hash_table_lookup (model->items_hash, command);
      if (G_LIKELY (item != NULL))
        {
          if (item->icon_large == NULL
              && item->item != NULL)
            {
              icon_name = garcon_menu_item_get_icon_name (item->item);
              item->icon_large = xfce_appfinder_model_load_pixbuf (icon_name, ICON_LARGE);
            }

          return g_object_ref (G_OBJECT (item->icon_large));
        }
    }

  return NULL;
}



void
xfce_appfinder_model_icon_theme_changed (XfceAppfinderModel *model)
{
  ModelItem   *item;
  GtkTreeIter  iter;
  GtkTreePath *path;
  gint         idx;
  gboolean     item_changed;
  GSList      *li;

  g_return_if_fail (XFCE_IS_APPFINDER_MODEL (model));

  APPFINDER_DEBUG ("icon theme changed");

  /* reload the command icons */
  if (model->command_icon_small != NULL)
    g_object_unref (G_OBJECT (model->command_icon_small));
  model->command_icon_small = xfce_appfinder_model_load_pixbuf (GTK_STOCK_EXECUTE, ICON_SMALL);

  if (model->command_icon_large != NULL)
    g_object_unref (G_OBJECT (model->command_icon_large));
  model->command_icon_large = xfce_appfinder_model_load_pixbuf (GTK_STOCK_EXECUTE, ICON_LARGE);

  /* update the model items */
  for (li = model->items, idx = 0; li != NULL; li = li->next, idx++)
    {
      item = li->data;
      item_changed = FALSE;

      if (item->icon_small != NULL)
        {
          g_object_unref (G_OBJECT (item->icon_small));
          item->icon_small = NULL;
          item_changed = TRUE;
        }
      if (item->icon_large != NULL)
        {
          g_object_unref (G_OBJECT (item->icon_large));
          item->icon_large = NULL;
          item_changed = TRUE;
        }

      if (item->item == NULL)
        {
          item->icon_small = g_object_ref (G_OBJECT (model->command_icon_small));
          item->icon_large = g_object_ref (G_OBJECT (model->command_icon_large));
        }

      if (item_changed)
        {
          path = gtk_tree_path_new_from_indices (idx, -1);
          ITER_INIT (iter, model->stamp, li);
          gtk_tree_model_row_changed (GTK_TREE_MODEL (model), path, &iter);
          gtk_tree_path_free (path);
        }
    }
}



GarconMenuDirectory *
xfce_appfinder_model_get_command_category (void)
{
  static GarconMenuDirectory *category = NULL;

  if (G_LIKELY (category != NULL))
    {
      g_object_ref (G_OBJECT (category));
    }
  else
    {
      category = g_object_new (GARCON_TYPE_MENU_DIRECTORY,
                               "name", _("Commands History"),
                               "icon-name", GTK_STOCK_EXECUTE,
                               NULL);
      g_object_add_weak_pointer (G_OBJECT (category), (gpointer) &category);
    }

  return category;
}