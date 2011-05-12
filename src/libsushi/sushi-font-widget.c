#include "sushi-font-widget.h"
#include "sushi-font-loader.h"

#include <cairo/cairo-ft.h>
#include <math.h>

enum {
  PROP_URI = 1,
  NUM_PROPERTIES
};

enum {
  LOADED,
  NUM_SIGNALS
};

struct _SushiFontWidgetPrivate {
  gchar *uri;

  FT_Face face;
  gchar *face_contents;

  const gchar *lowercase_text;
  const gchar *uppercase_text;
  const gchar *punctuation_text;

  gchar *sample_string;

  gchar *font_name;
  gboolean font_supports_title;
};

static GParamSpec *properties[NUM_PROPERTIES] = { NULL, };
static guint signals[NUM_SIGNALS] = { 0, };

G_DEFINE_TYPE (SushiFontWidget, sushi_font_widget, GTK_TYPE_DRAWING_AREA);

#define SECTION_SPACING 16

static const gchar lowercase_text_stock[] = "abcdefghijklmnopqrstuvwxyz";
static const gchar uppercase_text_stock[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const gchar punctuation_text_stock[] = "0123456789.:,;(*!?')";

/* adapted from gnome-utils:font-viewer/font-view.c */
static void
draw_string (cairo_t *cr,
             GtkBorder padding,
	     const gchar *text,
	     gint *pos_y)
{
  cairo_text_extents_t extents;
  gdouble cur_x, cur_y;

  cairo_text_extents (cr, text, &extents);

  if (pos_y != NULL)
    *pos_y += extents.height + extents.y_advance + padding.top;

  cairo_move_to (cr, padding.left, *pos_y);
  cairo_show_text (cr, text);

  *pos_y += padding.bottom;
}

static gboolean
check_font_contain_text (FT_Face face,
                         const gchar *text)
{
  gunichar *string;
  glong len, idx, map;
  FT_CharMap charmap;
  gboolean retval;

  string = g_utf8_to_ucs4_fast (text, -1, &len);

  for (map = 0; map < face->num_charmaps; map++) {
    charmap = face->charmaps[map];
    FT_Set_Charmap (face, charmap);

    retval = TRUE;

    for (idx = 0; idx < len; idx++) {
      gunichar c = string[idx];

      if (!FT_Get_Char_Index (face, c)) {
        retval = FALSE;
        break;
      }
    }

    if (retval)
      break;
  }

  g_free (string);

  return retval;
}

static gchar *
build_charlist_for_face (FT_Face face)
{
  GString *string;
  gulong c;
  guint glyph;  

  string = g_string_new (NULL);

  /* exclude normal ASCII characters here */
  c = 255;
  glyph = 0;

  do {
    c = FT_Get_Next_Char (face, c, &glyph);
    g_string_append_unichar (string, (gunichar) c);
  } while (glyph != 0);

  return g_string_free (string, FALSE);
}

static gchar *
random_string_from_available_chars (FT_Face face,
                                    gint n_chars)
{
  gchar *chars;
  gint idx, rand, total_chars;
  GString *retval;
  gchar *ptr, *end;

  idx = 0;
  chars = build_charlist_for_face (face);

  retval = g_string_new (NULL);
  total_chars = g_utf8_strlen (chars, -1);

  while (idx < n_chars) {
    rand = g_random_int_range (0, total_chars);

    ptr = g_utf8_offset_to_pointer (chars, rand);
    end = g_utf8_find_next_char (ptr, NULL);

    g_string_append_len (retval, ptr, end - ptr);
    idx++;
  }

  return g_string_free (retval, FALSE);
}

static void
build_strings_for_face (SushiFontWidget *self)
{
  const gchar *sample_string;

  /* if we don't have lowercase/uppercase/punctuation text in the face,
   * we omit it directly, and render a random text below.
   */
  if (check_font_contain_text (self->priv->face, lowercase_text_stock))
    self->priv->lowercase_text = lowercase_text_stock;
  else
    self->priv->lowercase_text = NULL;

  if (check_font_contain_text (self->priv->face, uppercase_text_stock))
    self->priv->uppercase_text = uppercase_text_stock;
  else
    self->priv->uppercase_text = NULL;

  if (check_font_contain_text (self->priv->face, punctuation_text_stock))
    self->priv->punctuation_text = punctuation_text_stock;
  else
    self->priv->punctuation_text = NULL;

  sample_string = pango_language_get_sample_string (NULL);

  if (check_font_contain_text (self->priv->face, sample_string))
    self->priv->sample_string = g_strdup (sample_string);
  else
    self->priv->sample_string = random_string_from_available_chars (self->priv->face, 36);

  self->priv->font_name =
    g_strconcat (self->priv->face->family_name, " ",
                 self->priv->face->style_name, NULL);
  self->priv->font_supports_title =
    check_font_contain_text (self->priv->face, self->priv->font_name);
}

static gint *
build_sizes_table (FT_Face face,
		   gint *n_sizes,
		   gint *alpha_size)
{
  gint *sizes = NULL;
  gint i;

  /* work out what sizes to render */
  if (FT_IS_SCALABLE (face)) {
    *n_sizes = 8;
    sizes = g_new (gint, *n_sizes);
    sizes[0] = 8;
    sizes[1] = 10;
    sizes[2] = 12;
    sizes[3] = 18;
    sizes[4] = 24;
    sizes[5] = 36;
    sizes[6] = 48;
    sizes[7] = 72;
    *alpha_size = 24;
  } else {
    /* use fixed sizes */
    *n_sizes = face->num_fixed_sizes;
    sizes = g_new (gint, *n_sizes);
    *alpha_size = 0;

    for (i = 0; i < face->num_fixed_sizes; i++) {
      sizes[i] = face->available_sizes[i].height;

      /* work out which font size to render */
      if (face->available_sizes[i].height <= 24)
        *alpha_size = face->available_sizes[i].height;
    }
  }

  return sizes;
}

static void
sushi_font_widget_size_request (GtkWidget *drawing_area,
                                gint *width,
                                gint *height)
{
  SushiFontWidget *self = SUSHI_FONT_WIDGET (drawing_area);
  SushiFontWidgetPrivate *priv = self->priv;
  gint i, pixmap_width, pixmap_height;
  cairo_text_extents_t extents;
  cairo_font_face_t *font;
  gint *sizes = NULL, n_sizes, alpha_size;
  cairo_t *cr;
  FT_Face face = priv->face;
  GtkStyleContext *context;
  GtkStateFlags state;
  GtkBorder padding;

  if (face == NULL) {
    if (width != NULL)
      *width = 1;
    if (height != NULL)
      *height = 1;

    return;
  }

  cr = gdk_cairo_create (gtk_widget_get_window (drawing_area));
  context = gtk_widget_get_style_context (drawing_area);
  state = gtk_style_context_get_state (context);
  gtk_style_context_get_padding (context, state, &padding);

  sizes = build_sizes_table (face, &n_sizes, &alpha_size);

  /* calculate size of pixmap to use */
  pixmap_width = padding.left + padding.right;
  pixmap_height = 0;

  font = cairo_ft_font_face_create_for_ft_face (face, 0);

  if (self->priv->font_supports_title)
    cairo_set_font_face (cr, font);

  cairo_set_font_size (cr, alpha_size + 6);
  cairo_text_extents (cr, self->priv->font_name, &extents);
  pixmap_height += extents.height + extents.y_advance + padding.top + padding.bottom;
  pixmap_width = MAX (pixmap_width, extents.width + padding.left + padding.right);

  if (!self->priv->font_supports_title)
    cairo_set_font_face (cr, font);

  cairo_font_face_destroy (font);

  pixmap_height += SECTION_SPACING / 2;

  cairo_set_font_size (cr, alpha_size);

  if (self->priv->lowercase_text != NULL) {
    cairo_text_extents (cr, self->priv->lowercase_text, &extents);
    pixmap_height += extents.height + extents.y_advance + padding.top + padding.bottom;
    pixmap_width = MAX (pixmap_width, extents.width + padding.left + padding.right);
  }

  if (self->priv->uppercase_text != NULL) {
    cairo_text_extents (cr, self->priv->uppercase_text, &extents);
    pixmap_height += extents.height + extents.y_advance + padding.top + padding.bottom;
    pixmap_width = MAX (pixmap_width, extents.width + padding.left + padding.right);
  }

  if (self->priv->punctuation_text != NULL) {
    cairo_text_extents (cr, self->priv->punctuation_text, &extents);
    pixmap_height += extents.height + extents.y_advance + padding.top + padding.bottom;
    pixmap_width = MAX (pixmap_width, extents.width + padding.left + padding.right);
  }

  pixmap_height += SECTION_SPACING;

  for (i = 0; i < n_sizes; i++) {
    cairo_set_font_size (cr, sizes[i]);
    cairo_text_extents (cr, self->priv->sample_string, &extents);
    pixmap_height += extents.height + extents.y_advance + padding.top + padding.bottom;
    pixmap_width = MAX (pixmap_width, extents.width + padding.left + padding.right);
  }

  pixmap_height += padding.bottom + SECTION_SPACING;

  if (width != NULL)
    *width = pixmap_width;

  if (height != NULL)
    *height = pixmap_height;

  cairo_destroy (cr);
  g_free (sizes);
}

static void
sushi_font_widget_get_preferred_width (GtkWidget *drawing_area,
                                       gint *minimum_width,
                                       gint *natural_width)
{
  gint width;

  sushi_font_widget_size_request (drawing_area, &width, NULL);

  *minimum_width = *natural_width = width;
}

static void
sushi_font_widget_get_preferred_height (GtkWidget *drawing_area,
                                        gint *minimum_height,
                                        gint *natural_height)
{
  gint height;

  sushi_font_widget_size_request (drawing_area, NULL, &height);

  *minimum_height = *natural_height = height;
}

static gboolean
sushi_font_widget_draw (GtkWidget *drawing_area,
                        cairo_t *cr)
{
  SushiFontWidget *self = SUSHI_FONT_WIDGET (drawing_area);
  SushiFontWidgetPrivate *priv = self->priv;
  gint *sizes = NULL, n_sizes, alpha_size, pos_y = 0, i;
  const gchar *text;
  cairo_font_face_t *font;
  FT_Face face = priv->face;
  gboolean res;
  GtkStyleContext *context;
  GdkRGBA color;
  GtkBorder padding;
  GtkStateFlags state;
  gint max_x = 0;
  gchar *font_name;

  if (face == NULL)
    goto end;

  context = gtk_widget_get_style_context (drawing_area);
  state = gtk_style_context_get_state (context);
  gtk_style_context_get_color (context, state, &color);
  gtk_style_context_get_padding (context, state, &padding);

  gdk_cairo_set_source_rgba (cr, &color);

  sizes = build_sizes_table (face, &n_sizes, &alpha_size);

  font = cairo_ft_font_face_create_for_ft_face (face, 0);

  if (self->priv->font_supports_title)
    cairo_set_font_face (cr, font);

  /* draw text */
  cairo_set_font_size (cr, alpha_size + 6);
  draw_string (cr, padding, self->priv->font_name, &pos_y);

  if (!self->priv->font_supports_title)
    cairo_set_font_face (cr, font);

  cairo_font_face_destroy (font);

  pos_y += SECTION_SPACING / 2;

  cairo_set_font_size (cr, alpha_size);

  if (self->priv->lowercase_text != NULL)
    draw_string (cr, padding, self->priv->lowercase_text, &pos_y);

  if (self->priv->uppercase_text != NULL)
    draw_string (cr, padding, self->priv->uppercase_text, &pos_y);

  if (self->priv->punctuation_text != NULL)
    draw_string (cr, padding, self->priv->punctuation_text, &pos_y);

  pos_y += SECTION_SPACING;

  for (i = 0; i < n_sizes; i++) {
    cairo_set_font_size (cr, sizes[i]);
    draw_string (cr, padding, self->priv->sample_string, &pos_y);
  }

 end:
  g_free (sizes);

  return FALSE;
}

static void
font_face_async_ready_cb (GObject *object,
                          GAsyncResult *result,
                          gpointer user_data)
{
  SushiFontWidget *self = user_data;
  FT_Face face;
  gchar *contents = NULL;
  GError *error = NULL;
  gint i, res;

  face = self->priv->face =
    sushi_new_ft_face_from_uri_finish (result,
                                       &self->priv->face_contents,
                                       &error);

  if (error != NULL) {
    /* FIXME: need to signal the error */
    g_print ("Can't load the font face: %s\n", error->message);
    return;
  }

  build_strings_for_face (self);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  g_signal_emit (self, signals[LOADED], 0);
}

static void
load_font_face (SushiFontWidget *self)
{
  sushi_new_ft_face_from_uri_async (self->priv->uri,
                                    font_face_async_ready_cb,
                                    self);
}

static void
sushi_font_widget_set_uri (SushiFontWidget *self,
                           const gchar *uri)
{
  g_free (self->priv->uri);
  self->priv->uri = g_strdup (uri);

  load_font_face (self);
}

static void
sushi_font_widget_init (SushiFontWidget *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, SUSHI_TYPE_FONT_WIDGET,
                                            SushiFontWidgetPrivate);

  self->priv->face = NULL;
}

static void
sushi_font_widget_get_property (GObject *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  SushiFontWidget *self = SUSHI_FONT_WIDGET (object);

  switch (prop_id) {
  case PROP_URI:
    g_value_set_string (value, self->priv->uri);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
sushi_font_widget_set_property (GObject *object,
                               guint       prop_id,
                               const GValue *value,
                               GParamSpec *pspec)
{
  SushiFontWidget *self = SUSHI_FONT_WIDGET (object);

  switch (prop_id) {
  case PROP_URI:
    sushi_font_widget_set_uri (self, g_value_get_string (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    break;
  }
}

static void
sushi_font_widget_finalize (GObject *object)
{
  SushiFontWidget *self = SUSHI_FONT_WIDGET (object);

  g_free (self->priv->uri);

  if (self->priv->face != NULL) {
    FT_Done_Face (self->priv->face);
    self->priv->face = NULL;
  }

  g_free (self->priv->font_name);
  g_free (self->priv->sample_string);
  g_free (self->priv->face_contents);

  G_OBJECT_CLASS (sushi_font_widget_parent_class)->finalize (object);
}

static void
sushi_font_widget_class_init (SushiFontWidgetClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS (klass);

  oclass->finalize = sushi_font_widget_finalize;
  oclass->set_property = sushi_font_widget_set_property;
  oclass->get_property = sushi_font_widget_get_property;

  wclass->draw = sushi_font_widget_draw;
  wclass->get_preferred_width = sushi_font_widget_get_preferred_width;
  wclass->get_preferred_height = sushi_font_widget_get_preferred_height;

  properties[PROP_URI] =
    g_param_spec_string ("uri",
                         "Uri", "Uri",
                         NULL, G_PARAM_READWRITE);

  signals[LOADED] =
    g_signal_new ("loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  g_object_class_install_properties (oclass, NUM_PROPERTIES, properties);
  g_type_class_add_private (klass, sizeof (SushiFontWidgetPrivate));
}

SushiFontWidget *
sushi_font_widget_new (const gchar *uri)
{
  return g_object_new (SUSHI_TYPE_FONT_WIDGET,
                       "uri", uri,
                       NULL);
}