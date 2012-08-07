
#include <cogl/cogl.h>
#include <cogl/cogl-gles2.h>
#include <string.h>

#include "test-utils.h"

typedef struct _TestState
{
  CoglTexture *offscreen_texture;
  CoglOffscreen *offscreen;
  CoglGLES2Context *gles2_ctx;
  const CoglGLES2Vtable *gles2;
} TestState;

static void
test_push_pop_single_context (void)
{
  CoglTexture *offscreen_texture;
  CoglOffscreen *offscreen;
  CoglPipeline *pipeline;
  CoglGLES2Context *gles2_ctx;
  const CoglGLES2Vtable *gles2;
  GError *error = NULL;

  offscreen_texture = COGL_TEXTURE (
    cogl_texture_2d_new_with_size (ctx,
                                   cogl_framebuffer_get_width (fb),
                                   cogl_framebuffer_get_height (fb),
                                   COGL_PIXEL_FORMAT_ANY,
                                   NULL));
  offscreen = cogl_offscreen_new_to_texture (offscreen_texture);

  pipeline = cogl_pipeline_new (ctx);
  cogl_pipeline_set_layer_texture (pipeline, 0, offscreen_texture);

  gles2_ctx = cogl_gles2_context_new (ctx, &error);
  if (!gles2_ctx)
    g_error ("Failed to create GLES2 context: %s\n", error->message);

  gles2 = cogl_gles2_context_get_vtable (gles2_ctx);

  /* Clear onscreen to 0xffff00 using GLES2 */

  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                fb,
                                fb,
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glClearColor (1, 1, 0, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);

  cogl_pop_gles2_context (ctx);

  test_utils_check_pixel (fb, 0, 0, 0xffff00ff);

  /* Clear offscreen to 0xff0000 using GLES2 and then copy the result
   * onscreen.
   *
   * If we fail to bind the new context here then we'd probably end up
   * clearing onscreen to 0xff0000 and copying 0xffff00 to onscreen
   * instead.
   */

  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                COGL_FRAMEBUFFER (offscreen),
                                COGL_FRAMEBUFFER (offscreen),
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glClearColor (1, 0, 0, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);

  cogl_pop_gles2_context (ctx);

  cogl_framebuffer_draw_rectangle (fb,
                                   pipeline,
                                   -1, 1, 1, -1);
  /* NB: Cogl doesn't automatically support mid-scene modifications
   * of textures and so we explicitly flush the drawn rectangle to the
   * framebuffer now otherwise it may be batched until after the
   * offscreen texture has been modified again. */
  cogl_flush ();

  /* Clear the offscreen framebuffer to blue using GLES2 before
   * reading back from the onscreen framebuffer in case we mistakenly
   * read from the offscreen framebuffer and get a false positive
   */
  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                COGL_FRAMEBUFFER (offscreen),
                                COGL_FRAMEBUFFER (offscreen),
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glClearColor (0, 0, 1, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);

  cogl_pop_gles2_context (ctx);

  test_utils_check_pixel (fb, 0, 0, 0xff0000ff);

  /* Now copy the offscreen blue clear to the onscreen framebufer and
   * check that too */
  cogl_framebuffer_draw_rectangle (fb,
                                   pipeline,
                                   -1, 1, 1, -1);

  test_utils_check_pixel (fb, 0, 0, 0x0000ffff);

  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                fb,
                                fb,
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glClearColor (1, 0, 1, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);

  cogl_pop_gles2_context (ctx);

  test_utils_check_pixel (fb, 0, 0, 0xff00ffff);


  cogl_object_unref (gles2_ctx);

  cogl_object_unref (pipeline);
}

static void
create_gles2_context (CoglTexture **offscreen_texture,
                      CoglOffscreen **offscreen,
                      CoglPipeline **pipeline,
                      CoglGLES2Context **gles2_ctx,
                      const CoglGLES2Vtable **gles2)
{
  GError *error = NULL;

  *offscreen_texture = COGL_TEXTURE (
    cogl_texture_2d_new_with_size (ctx,
                                   cogl_framebuffer_get_width (fb),
                                   cogl_framebuffer_get_height (fb),
                                   COGL_PIXEL_FORMAT_ANY,
                                   NULL));
  *offscreen = cogl_offscreen_new_to_texture (*offscreen_texture);

  *pipeline = cogl_pipeline_new (ctx);
  cogl_pipeline_set_layer_texture (*pipeline, 0, *offscreen_texture);

  *gles2_ctx = cogl_gles2_context_new (ctx, &error);
  if (!*gles2_ctx)
    g_error ("Failed to create GLES2 context: %s\n", error->message);

  *gles2 = cogl_gles2_context_get_vtable (*gles2_ctx);
}

static void
test_push_pop_multi_context (void)
{
  CoglTexture *offscreen_texture0;
  CoglOffscreen *offscreen0;
  CoglPipeline *pipeline0;
  CoglGLES2Context *gles2_ctx0;
  const CoglGLES2Vtable *gles20;
  CoglTexture *offscreen_texture1;
  CoglOffscreen *offscreen1;
  CoglPipeline *pipeline1;
  CoglGLES2Context *gles2_ctx1;
  const CoglGLES2Vtable *gles21;
  GError *error = NULL;

  create_gles2_context (&offscreen_texture0,
                        &offscreen0,
                        &pipeline0,
                        &gles2_ctx0,
                        &gles20);

  create_gles2_context (&offscreen_texture1,
                        &offscreen1,
                        &pipeline1,
                        &gles2_ctx1,
                        &gles21);

  cogl_framebuffer_clear4f (fb, COGL_BUFFER_BIT_COLOR, 1, 1, 1, 1);

  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx0,
                                COGL_FRAMEBUFFER (offscreen0),
                                COGL_FRAMEBUFFER (offscreen0),
                                &error))
    {
      g_error ("Failed to push gles2 context 0: %s\n", error->message);
    }

  gles20->glClearColor (1, 0, 0, 1);
  gles20->glClear (GL_COLOR_BUFFER_BIT);

  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx1,
                                COGL_FRAMEBUFFER (offscreen1),
                                COGL_FRAMEBUFFER (offscreen1),
                                &error))
    {
      g_error ("Failed to push gles2 context 1: %s\n", error->message);
    }

  gles21->glClearColor (0, 1, 0, 1);
  gles21->glClear (GL_COLOR_BUFFER_BIT);

  cogl_pop_gles2_context (ctx);
  cogl_pop_gles2_context (ctx);

  test_utils_check_pixel (fb, 0, 0, 0xffffffff);

  cogl_framebuffer_draw_rectangle (fb,
                                   pipeline0,
                                   -1, 1, 1, -1);

  test_utils_check_pixel (fb, 0, 0, 0xff0000ff);

  cogl_framebuffer_draw_rectangle (fb,
                                   pipeline1,
                                   -1, 1, 1, -1);

  test_utils_check_pixel (fb, 0, 0, 0x00ff00ff);
}

static GLuint
create_gles2_framebuffer (const CoglGLES2Vtable *gles2,
                          int width,
                          int height)
{
  GLuint texture_handle;
  GLuint fbo_handle;
  GLenum status;

  gles2->glGenTextures (1, &texture_handle);
  gles2->glGenFramebuffers (1, &fbo_handle);

  gles2->glBindTexture (GL_TEXTURE_2D, texture_handle);
  gles2->glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gles2->glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gles2->glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, NULL);
  gles2->glBindTexture (GL_TEXTURE_2D, 0);

  gles2->glBindFramebuffer (GL_FRAMEBUFFER, fbo_handle);
  gles2->glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, texture_handle, 0);

  status = gles2->glCheckFramebufferStatus (GL_FRAMEBUFFER);
  if (cogl_test_verbose ())
    g_print ("status for gles2 framebuffer = 0x%x %s\n",
             status, status == GL_FRAMEBUFFER_COMPLETE ? "(complete)" : "(?)");

  gles2->glBindFramebuffer (GL_FRAMEBUFFER, 0);

  return fbo_handle;
}

static void
test_gles2_read_pixels (void)
{
  CoglTexture *offscreen_texture;
  CoglOffscreen *offscreen;
  CoglPipeline *pipeline;
  CoglGLES2Context *gles2_ctx;
  const CoglGLES2Vtable *gles2;
  GError *error = NULL;
  GLubyte pixel[3];
  GLuint fbo_handle;

  create_gles2_context (&offscreen_texture,
                        &offscreen,
                        &pipeline,
                        &gles2_ctx,
                        &gles2);

  cogl_framebuffer_clear4f (fb, COGL_BUFFER_BIT_COLOR, 1, 1, 1, 1);

  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                COGL_FRAMEBUFFER (offscreen),
                                COGL_FRAMEBUFFER (offscreen),
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glClearColor (1, 0, 0, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);
  gles2->glReadPixels (0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, &pixel);

  g_assert (pixel[0] == 0xff);
  g_assert (pixel[1] == 0);
  g_assert (pixel[2] == 0);

  fbo_handle = create_gles2_framebuffer (gles2, 256, 256);

  gles2->glBindFramebuffer (GL_FRAMEBUFFER, fbo_handle);

  gles2->glClearColor (0, 1, 0, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);
  gles2->glReadPixels (0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, &pixel);

  g_assert (pixel[0] == 0);
  g_assert (pixel[1] == 0xff);
  g_assert (pixel[2] == 0);

  gles2->glBindFramebuffer (GL_FRAMEBUFFER, 0);

  gles2->glClearColor (0, 1, 1, 1);
  gles2->glClear (GL_COLOR_BUFFER_BIT);
  gles2->glReadPixels (0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, &pixel);

  g_assert (pixel[0] == 0);
  g_assert (pixel[1] == 0xff);
  g_assert (pixel[2] == 0xff);

  cogl_pop_gles2_context (ctx);

  test_utils_check_pixel (fb, 0, 0, 0xffffffff);

  /* Bind different read and write buffers */
  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                COGL_FRAMEBUFFER (offscreen),
                                fb,
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glReadPixels (0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, &pixel);

  g_assert (pixel[0] == 0);
  g_assert (pixel[1] == 0xff);
  g_assert (pixel[2] == 0xff);

  cogl_pop_gles2_context (ctx);

  test_utils_check_pixel (fb, 0, 0, 0xffffffff);

  /* Bind different read and write buffers (the other way around from
   * before so when we test with COGL_TEST_ONSCREEN=1 we will read
   * from an onscreen framebuffer) */
  if (!cogl_push_gles2_context (ctx,
                                gles2_ctx,
                                fb,
                                COGL_FRAMEBUFFER (offscreen),
                                &error))
    {
      g_error ("Failed to push gles2 context: %s\n", error->message);
    }

  gles2->glReadPixels (0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, &pixel);

  g_assert (pixel[0] == 0xff);
  g_assert (pixel[1] == 0xff);
  g_assert (pixel[2] == 0xff);

  cogl_pop_gles2_context (ctx);
}

void
test_gles2_context (void)
{
  test_push_pop_single_context ();
  test_push_pop_multi_context ();
  test_gles2_read_pixels ();

  if (cogl_test_verbose ())
    g_print ("OK\n");
}

static GLuint
create_shader (const CoglGLES2Vtable *gles2,
               GLenum type,
               const char *source)
{
  GLuint shader;
  GLint status;
  int length = strlen (source);

  shader = gles2->glCreateShader (type);
  gles2->glShaderSource (shader, 1, &source, &length);
  gles2->glCompileShader (shader);
  gles2->glGetShaderiv (shader, GL_COMPILE_STATUS, &status);

  if (!status)
    {
      char buf[512];

      gles2->glGetShaderInfoLog (shader, sizeof (buf), NULL, buf);

      g_error ("Shader compilation failed:\n%s", buf);
    }

  return shader;
}

static GLuint
create_program (const CoglGLES2Vtable *gles2,
                const char *vertex_shader_source,
                const char *fragment_shader_source)
{
  GLuint fragment_shader, vertex_shader, program;
  GLint status;

  vertex_shader =
    create_shader (gles2, GL_VERTEX_SHADER, vertex_shader_source);
  fragment_shader =
    create_shader (gles2, GL_FRAGMENT_SHADER, fragment_shader_source);

  program = gles2->glCreateProgram ();
  gles2->glAttachShader (program, vertex_shader);
  gles2->glAttachShader (program, fragment_shader);
  gles2->glLinkProgram (program);

  gles2->glGetProgramiv (program, GL_LINK_STATUS, &status);

  if (!status)
    {
      char buf[512];

      gles2->glGetProgramInfoLog (program, sizeof (buf), NULL, buf);

      g_error ("Program linking failed:\n%s", buf);
    }

  return program;
}

typedef struct
{
  const CoglGLES2Vtable *gles2;
  GLint color_location;
  GLint pos_location;
  int fb_width, fb_height;
} PaintData;

typedef void (* PaintMethod) (PaintData *data);

/* Top vertices are counter-clockwise */
static const float top_vertices[] =
  {
    -1.0f, 0.0f,
    1.0f, 0.0f,
    -1.0f, 1.0f,
    1.0f, 1.0f
  };
/* Bottom vertices are clockwise */
static const float bottom_vertices[] =
  {
    1.0f, 0.0f,
    1.0f, -1.0f,
    -1.0f, 0.0f,
    -1.0f, -1.0f
  };

static void
paint_quads (PaintData *data)
{
  const CoglGLES2Vtable *gles2 = data->gles2;

  gles2->glEnableVertexAttribArray (data->pos_location);

  /* Paint the top half in red */
  gles2->glUniform4f (data->color_location,
                      1.0f, 0.0f, 0.0f, 1.0f);
  gles2->glVertexAttribPointer (data->pos_location,
                                2, /* size */
                                GL_FLOAT,
                                GL_FALSE, /* not normalized */
                                sizeof (float) * 2,
                                top_vertices);
  gles2->glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

  /* Paint the bottom half in blue */
  gles2->glUniform4f (data->color_location,
                      0.0f, 0.0f, 1.0f, 1.0f);
  gles2->glVertexAttribPointer (data->pos_location,
                                2, /* size */
                                GL_FLOAT,
                                GL_FALSE, /* not normalized */
                                sizeof (float) * 2,
                                bottom_vertices);
  gles2->glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
}

static void
paint_viewport (PaintData *data)
{
  const CoglGLES2Vtable *gles2 = data->gles2;
  int viewport[4];

  /* Vertices to fill the entire framebuffer */
  static const float vertices[] =
    {
      -1.0f, -1.0f,
      1.0f, -1.0f,
      -1.0f, 1.0f,
      1.0f, 1.0f
    };

  gles2->glEnableVertexAttribArray (data->pos_location);
  gles2->glVertexAttribPointer (data->pos_location,
                                2, /* size */
                                GL_FLOAT,
                                GL_FALSE, /* not normalized */
                                sizeof (float) * 2,
                                vertices);

  /* Paint the top half in red */
  gles2->glViewport (0, data->fb_height / 2,
                     data->fb_width, data->fb_height / 2);
  gles2->glUniform4f (data->color_location,
                      1.0f, 0.0f, 0.0f, 1.0f);
  gles2->glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

  /* Paint the bottom half in blue */
  gles2->glViewport (0, 0, data->fb_width, data->fb_height / 2);
  gles2->glUniform4f (data->color_location,
                      0.0f, 0.0f, 1.0f, 1.0f);
  gles2->glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

  gles2->glGetIntegerv (GL_VIEWPORT, viewport);
  g_assert_cmpint (viewport[0], ==, 0.0f);
  g_assert_cmpint (viewport[1], ==, 0.0f);
  g_assert_cmpint (viewport[2], ==, data->fb_width);
  g_assert_cmpint (viewport[3], ==, data->fb_height / 2);
}

static void
paint_scissor (PaintData *data)
{
  const CoglGLES2Vtable *gles2 = data->gles2;
  float scissor[4];

  gles2->glEnable (GL_SCISSOR_TEST);

  /* Paint the top half in red */
  gles2->glScissor (0, data->fb_height / 2,
                    data->fb_width, data->fb_height / 2);
  gles2->glClearColor (1.0, 0.0, 0.0, 1.0);
  gles2->glClear (GL_COLOR_BUFFER_BIT);

  /* Paint the bottom half in blue */
  gles2->glScissor (0, 0, data->fb_width, data->fb_height / 2);
  gles2->glClearColor (0.0, 0.0, 1.0, 1.0);
  gles2->glClear (GL_COLOR_BUFFER_BIT);

  gles2->glGetFloatv (GL_SCISSOR_BOX, scissor);
  g_assert_cmpfloat (scissor[0], ==, 0.0f);
  g_assert_cmpfloat (scissor[1], ==, 0.0f);
  g_assert_cmpfloat (scissor[2], ==, data->fb_width);
  g_assert_cmpfloat (scissor[3], ==, data->fb_height / 2);
}

static void
paint_cull (PaintData *data)
{
  const CoglGLES2Vtable *gles2 = data->gles2;
  GLint front_face;
  int i;

  gles2->glEnableVertexAttribArray (data->pos_location);
  gles2->glEnable (GL_CULL_FACE);

  /* First time round we'll use GL_CCW as the front face so that the
   * bottom quad will be culled */
  gles2->glFrontFace (GL_CCW);
  gles2->glUniform4f (data->color_location,
                      1.0f, 0.0f, 0.0f, 1.0f);

  gles2->glGetIntegerv (GL_FRONT_FACE, &front_face);
  g_assert_cmpint (front_face, ==, GL_CCW);

  for (i = 0; i < 2; i++)
    {
      /* Paint both quads in the same color. One of these will be
       * culled */
      gles2->glVertexAttribPointer (data->pos_location,
                                    2, /* size */
                                    GL_FLOAT,
                                    GL_FALSE, /* not normalized */
                                    sizeof (float) * 2,
                                    top_vertices);
      gles2->glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

      gles2->glVertexAttribPointer (data->pos_location,
                                    2, /* size */
                                    GL_FLOAT,
                                    GL_FALSE, /* not normalized */
                                    sizeof (float) * 2,
                                    bottom_vertices);
      gles2->glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

      /* Second time round we'll use GL_CW as the front face so that the
       * top quad will be culled */
      gles2->glFrontFace (GL_CW);
      gles2->glUniform4f (data->color_location,
                          0.0f, 0.0f, 1.0f, 1.0f);

      gles2->glGetIntegerv (GL_FRONT_FACE, &front_face);
      g_assert_cmpint (front_face, ==, GL_CW);
    }
}

static void
verify_read_pixels (const PaintData *data)
{
  int stride = data->fb_width * 4;
  uint8_t *buf = g_malloc (data->fb_height * stride);

  data->gles2->glReadPixels (0, 0, /* x/y */
                             data->fb_width, data->fb_height,
                             GL_RGBA,
                             GL_UNSIGNED_BYTE,
                             buf);

  /* In GL, the lines earlier in the buffer are the bottom */
  /* Bottom should be blue */
  test_utils_compare_pixel (buf + data->fb_width / 2 * 4 +
                            data->fb_height / 4 * stride,
                            0x0000ffff);
  /* Top should be red */
  test_utils_compare_pixel (buf + data->fb_width / 2 * 4 +
                            data->fb_height * 3 / 4 * stride,
                            0xff0000ff);

  g_free (buf);
}

void
test_gles2_context_fbo (void)
{
  static const char vertex_shader_source[] =
    "attribute vec2 pos;\n"
    "\n"
    "void\n"
    "main ()\n"
    "{\n"
    "  gl_Position = vec4 (pos, 0.0, 1.0);\n"
    "}\n";
  static const char fragment_shader_source[] =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "\n"
    "void\n"
    "main ()\n"
    "{\n"
    "  gl_FragColor = color;\n"
    "}\n";
  static const PaintMethod paint_methods[] =
    {
      paint_quads,
      paint_viewport,
      paint_scissor,
      paint_cull
    };
  int i;
  PaintData data;

  data.fb_width = cogl_framebuffer_get_width (fb);
  data.fb_height = cogl_framebuffer_get_height (fb);

  for (i = 0; i < G_N_ELEMENTS (paint_methods); i++)
    {
      CoglTexture *offscreen_texture;
      CoglOffscreen *offscreen;
      CoglPipeline *pipeline;
      CoglGLES2Context *gles2_ctx;
      GLuint program;
      GError *error = NULL;

      create_gles2_context (&offscreen_texture,
                            &offscreen,
                            &pipeline,
                            &gles2_ctx,
                            &data.gles2);

      if (!cogl_push_gles2_context (ctx,
                                    gles2_ctx,
                                    COGL_FRAMEBUFFER (offscreen),
                                    COGL_FRAMEBUFFER (offscreen),
                                    &error))
        g_error ("Failed to push gles2 context: %s\n", error->message);

      program = create_program (data.gles2,
                                vertex_shader_source,
                                fragment_shader_source);

      data.gles2->glClearColor (1.0, 1.0, 0.0, 1.0);
      data.gles2->glClear (GL_COLOR_BUFFER_BIT);

      data.gles2->glUseProgram (program);

      data.color_location = data.gles2->glGetUniformLocation (program, "color");
      if (data.color_location == -1)
        g_error ("Couldn't find ‘color’ uniform");

      data.pos_location = data.gles2->glGetAttribLocation (program, "pos");
      if (data.pos_location == -1)
        g_error ("Couldn't find ‘pos’ attribute");

      paint_methods[i] (&data);

      verify_read_pixels (&data);

      cogl_pop_gles2_context (ctx);

      cogl_object_unref (offscreen);
      cogl_object_unref (gles2_ctx);

      cogl_framebuffer_draw_rectangle (fb,
                                       pipeline,
                                       -1.0f, 1.0f,
                                       1.0f, -1.0f);

      cogl_object_unref (pipeline);
      cogl_object_unref (offscreen_texture);

      /* Top half of the framebuffer should be red */
      test_utils_check_pixel (fb,
                              data.fb_width / 2, data.fb_height / 4,
                              0xff0000ff);
      /* Bottom half should be blue */
      test_utils_check_pixel (fb,
                              data.fb_width / 2, data.fb_height * 3 / 4,
                              0x0000ffff);
    }
}
