#include "sushi-font-loader.h"

#include <stdlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <gio/gio.h>

typedef struct {
  FT_Library library;
  FT_Long face_index;
  GFile *file;
  GSimpleAsyncResult *result;

  gchar *face_contents;
  gsize face_length;
} FontLoadJob;

static FontLoadJob *
font_load_job_new (const gchar *uri,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
  FontLoadJob *job = NULL;
  FT_Library library;
  FT_Error res;
  GError *error = NULL;

  res = FT_Init_FreeType (&library);

  if (res != 0) {
    g_set_error_literal (&error,
                         G_IO_ERROR, 0,
                         "Can't initialize FreeType");
    g_simple_async_report_take_gerror_in_idle (NULL,
                                               callback, user_data,
                                               error);
    goto out;
  }

  job = g_slice_new0 (FontLoadJob);

  job->library = library;
  job->face_index = 0;
  job->file = g_file_new_for_uri (uri);
  job->result = g_simple_async_result_new
    (NULL, callback, user_data,
     sushi_new_ft_face_from_uri_async);

  g_simple_async_result_set_op_res_gpointer (job->result, job, NULL);

 out:
  return job;
}

static void
font_load_job_free (FontLoadJob *job)
{
  g_clear_object (&job->result);
  g_clear_object (&job->file);

  g_slice_free (FontLoadJob, job);
}

static FT_Face
create_face_from_contents (FontLoadJob *job,
                           gchar **contents,
                           GError **error)
{
  FT_Error ft_error;
  FT_Face retval;

  ft_error = FT_New_Memory_Face (job->library,
                                 job->face_contents,
                                 (FT_Long) job->face_length,
                                 job->face_index,
                                 &retval);

  if (ft_error != 0) {
    g_set_error_literal (error, G_IO_ERROR, 0,
                         "Unable to read the font face file");
    retval = NULL;
    g_free (job->face_contents);
  } else {
    *contents = job->face_contents;
  }

  return retval;
}

static gboolean
font_load_job_callback (gpointer user_data)
{
  FontLoadJob *job = user_data;

  g_simple_async_result_complete (job->result);
  font_load_job_free (job);

  return FALSE;
}

static gboolean
font_load_job (GIOSchedulerJob *sched_job,
               GCancellable *cancellable,
               gpointer user_data)
{
  FontLoadJob *job = user_data;
  GError *error = NULL;
  gchar *contents;
  gsize length;
  gboolean res;

  res = g_file_load_contents (job->file, NULL,
                              &contents, &length, NULL, &error);

  if (error != NULL) {
    g_simple_async_result_take_error (job->result, error);
  } else {
    job->face_contents = contents;
    job->face_length = length;
  }

  g_io_scheduler_job_send_to_mainloop_async (sched_job,
                                             font_load_job_callback,
                                             job, NULL);

  return FALSE;
}

void
sushi_new_ft_face_from_uri_async (const gchar *uri,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  FontLoadJob *job = NULL;

  job = font_load_job_new (uri, callback, user_data);

  if (!job)
    return;

  g_io_scheduler_push_job (font_load_job,
                           job, NULL,
                           G_PRIORITY_DEFAULT,
                           NULL);
}

FT_Face
sushi_new_ft_face_from_uri_finish (GAsyncResult *result,
                                   gchar **contents,
                                   GError **error)
{
  FontLoadJob *job;

  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
                                             error))
    return NULL;

  job = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));

  return create_face_from_contents (job, contents, error);
}
