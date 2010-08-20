/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2007,2008,2009,2010 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl.h"
#include "cogl-shader-private.h"
#include "cogl-internal.h"
#include "cogl-context.h"
#include "cogl-handle.h"

#include <glib.h>

#include <string.h>

#ifdef HAVE_COGL_GL
#define glCreateShader      ctx->drv.pf_glCreateShader
#define glGetShaderiv       ctx->drv.pf_glGetShaderiv
#define glGetShaderInfoLog  ctx->drv.pf_glGetShaderInfoLog
#define glCompileShader     ctx->drv.pf_glCompileShader
#define glShaderSource      ctx->drv.pf_glShaderSource
#define glDeleteShader      ctx->drv.pf_glDeleteShader
#define GET_CONTEXT         _COGL_GET_CONTEXT
#else
#define GET_CONTEXT(CTXVAR,RETVAL) G_STMT_START { } G_STMT_END
#endif

#ifndef HAVE_COGL_GLES

static void _cogl_shader_free (CoglShader *shader);

COGL_HANDLE_DEFINE (Shader, shader);
COGL_OBJECT_DEFINE_DEPRECATED_REF_COUNTING (shader);

static void
_cogl_shader_free (CoglShader *shader)
{
  /* Frees shader resources but its handle is not
     released! Do that separately before this! */
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

#ifdef HAVE_COGL_GL
  if (shader->language == COGL_SHADER_LANGUAGE_ARBFP)
    g_free (shader->arbfp_source);
  else
#endif
    if (shader->gl_handle)
      GE (glDeleteShader (shader->gl_handle));

  g_slice_free (CoglShader, shader);
}

CoglHandle
cogl_create_shader (CoglShaderType type)
{
  CoglShader *shader;

  GET_CONTEXT (ctx, COGL_INVALID_HANDLE);

  switch (type)
    {
    case COGL_SHADER_TYPE_VERTEX:
    case COGL_SHADER_TYPE_FRAGMENT:
      break;
    default:
      g_warning ("Unexpected shader type (0x%08lX) given to "
                 "cogl_create_shader", (unsigned long) type);
      return COGL_INVALID_HANDLE;
    }

  shader = g_slice_new (CoglShader);
  shader->language = COGL_SHADER_LANGUAGE_GLSL;
  shader->gl_handle = 0;
  shader->type = type;

  return _cogl_shader_handle_new (shader);
}

void
cogl_shader_source (CoglHandle   handle,
                    const char  *source)
{
  CoglShader *shader;
  CoglShaderLanguage language;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (!cogl_is_shader (handle))
    return;

  shader = _cogl_shader_pointer_from_handle (handle);

#ifdef HAVE_COGL_GL
  if (strncmp (source, "!!ARBfp1.0", 10) == 0)
    language = COGL_SHADER_LANGUAGE_ARBFP;
  else
#endif
    language = COGL_SHADER_LANGUAGE_GLSL;

  /* Delete the old object if the language is changing... */
  if (G_UNLIKELY (language != shader->language))
    {
#ifdef HAVE_COGL_GL

      if (shader->language == COGL_SHADER_LANGUAGE_ARBFP)
        {
          g_free (shader->arbfp_source);
          shader->arbfp_source = NULL;
        }
      else
#endif
        {
          if (shader->gl_handle)
            GE (glDeleteShader (shader->gl_handle));
        }
    }

#ifdef HAVE_COGL_GL
  if (language == COGL_SHADER_LANGUAGE_ARBFP)
      shader->arbfp_source = g_strdup (source);
  else
#endif
    {
      if (!shader->gl_handle)
        {
          GLenum gl_type;

          switch (shader->type)
            {
            case COGL_SHADER_TYPE_VERTEX:
              gl_type = GL_VERTEX_SHADER;
              break;
            case COGL_SHADER_TYPE_FRAGMENT:
              gl_type = GL_FRAGMENT_SHADER;
              break;
            default:
              g_assert_not_reached ();
              break;
            }

          shader->gl_handle = glCreateShader (gl_type);
        }
      glShaderSource (shader->gl_handle, 1, &source, NULL);
    }

  shader->language = language;
}

void
cogl_shader_compile (CoglHandle handle)
{
  CoglShader *shader;
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  if (!cogl_is_shader (handle))
    return;

  shader = _cogl_shader_pointer_from_handle (handle);

  if (shader->language == COGL_SHADER_LANGUAGE_GLSL)
    GE (glCompileShader (shader->gl_handle));
}

char *
cogl_shader_get_info_log (CoglHandle handle)
{
  CoglShader *shader;

  GET_CONTEXT (ctx, NULL);

  if (!cogl_is_shader (handle))
    return NULL;

  shader = _cogl_shader_pointer_from_handle (handle);

#ifdef HAVE_COGL_GL
  if (shader->language == COGL_SHADER_LANGUAGE_ARBFP)
    {
      /* ARBfp exposes a program error string, but since cogl_program
       * doesn't have any API to query an error log it is not currently
       * exposed. */
      return g_strdup ("");
    }
  else
#endif
    {
      char buffer[512];
      int len = 0;
      glGetShaderInfoLog (shader->gl_handle, 511, &len, buffer);
      buffer[len] = '\0';
      return g_strdup (buffer);
    }
}

CoglShaderType
cogl_shader_get_type (CoglHandle  handle)
{
  CoglShader *shader;

  GET_CONTEXT (ctx, COGL_SHADER_TYPE_VERTEX);

  if (!cogl_is_shader (handle))
    {
      g_warning ("Non shader handle type passed to cogl_shader_get_type");
      return COGL_SHADER_TYPE_VERTEX;
    }

  shader = _cogl_shader_pointer_from_handle (handle);
  return shader->type;
}

gboolean
cogl_shader_is_compiled (CoglHandle handle)
{
  GLint status;
  CoglShader *shader;

  GET_CONTEXT (ctx, FALSE);

  if (!cogl_is_shader (handle))
    return FALSE;

  shader = _cogl_shader_pointer_from_handle (handle);

#ifdef HAVE_COGL_GL
  if (shader->language == COGL_SHADER_LANGUAGE_ARBFP)
    return TRUE;
  else
#endif
    {
      GE (glGetShaderiv (shader->gl_handle, GL_COMPILE_STATUS, &status));
      if (status == GL_TRUE)
        return TRUE;
      else
        return FALSE;
    }
}

#else /* HAVE_COGL_GLES */

/* No support on regular OpenGL 1.1 */

CoglHandle
cogl_create_shader (CoglShaderType type)
{
  return COGL_INVALID_HANDLE;
}

gboolean
cogl_is_shader (CoglHandle handle)
{
  return FALSE;
}

CoglHandle
cogl_shader_ref (CoglHandle handle)
{
  return COGL_INVALID_HANDLE;
}

void
cogl_shader_unref (CoglHandle handle)
{
}

void
cogl_shader_source (CoglHandle  shader,
                    const char   *source)
{
}

void
cogl_shader_compile (CoglHandle shader_handle)
{
}

char *
cogl_shader_get_info_log (CoglHandle handle)
{
  return NULL;
}

CoglShaderType
cogl_shader_get_type (CoglHandle  handle)
{
  return COGL_SHADER_TYPE_VERTEX;
}

gboolean
cogl_shader_is_compiled (CoglHandle handle)
{
  return FALSE;
}

#endif /* HAVE_COGL_GLES */
