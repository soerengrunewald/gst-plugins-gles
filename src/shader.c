/*
 * GStreamer
 * Copyright (C) 2011 Julian Scheel <julian@jusst.de>
 * Copyright (C) 2011 Soeren Grunewald <soeren.grunewald@avionic-design.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>

#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <GLES2/gl2.h>

#include "shader.h"
#include "gstglessink.h"

/* FIXME: Should be part of the GLES headers */
#define GL_NVIDIA_PLATFORM_BINARY_NV                            0x890B

GST_DEBUG_CATEGORY_EXTERN (gst_gles_sink_debug);


static const gchar* shader_basenames[] = {
    "deint_linear", /* SHADER_DEINT_LINEAR */
    "copy" /* SHADER_COPY, simple linear scaled copy shader */
};

#ifndef DATA_DIR
#define DATA_DIR "/usr/share/gst-plugins-gles/shaders"
#endif

#define SHADER_EXT_BINARY ".glsh"
#define SHADER_EXT_SOURCE ".glsl"

#define VERTEX_SHADER_BASENAME "vertex"

static gboolean gl_extension_available(const gchar *extension)
{
    const gchar *gl_extensions = (gchar*)glGetString(GL_EXTENSIONS);
    return (g_strstr_len(gl_extensions, -1, extension) != NULL);
}

static GLuint
gl_load_binary_shader (GstElement *sink, const char *filename,
                       GLenum type)
{
    GLbyte *binary = NULL;
    GFile *file = NULL;
    GLuint shader = 0;
    GLsizei length;
    GLint err;

    if (!gl_extension_available("GL_NV_platform_binary")) {
        GST_WARNING_OBJECT(sink, "Binary shaders are not supported, "
                           "falling back to source shaders.");
        return 0;
    }

    file = g_file_new_for_path (filename);
    /* create a shader object */
    shader = glCreateShader (type);
    if (shader == 0) {
        GST_ERROR_OBJECT(sink, "Could not create shader object");
        goto cleanup;
    }

    if (!g_file_load_contents (file, NULL, (char**)&binary,
                               (gsize*)&length, NULL, NULL)) {
        GST_ERROR_OBJECT(sink, "Could not read binary shader from %s",
                         filename);
        glDeleteShader (shader);
        shader = 0;
        goto cleanup;
    }

    glShaderBinary (1, &shader, GL_NVIDIA_PLATFORM_BINARY_NV,
                    binary, length);

    err = glGetError ();
    if (err != GL_NO_ERROR) {
        GST_ERROR_OBJECT (sink, "Error loading binary shader: %d\n", err);
        glDeleteShader (shader);
        shader = 0;
    }

cleanup:
    g_free (binary);
    g_object_unref (file);
    return shader;
}

/* load and compile a shader src into a shader program */
static GLuint
gl_load_source_shader (GstElement *sink, const char *shader_filename,
                       GLenum type)
{
    GFile *shader_file;
    GLuint shader = 0;
    char *shader_src;
    GLint compiled;
    gsize src_len;
    GError *err;

    /* create a shader object */
    shader = glCreateShader (type);
    if (shader == 0) {
        GST_ERROR_OBJECT (sink, "Could not create shader object");
        return 0;
    }

    /* read shader source from file */
    shader_file = g_file_new_for_path (shader_filename);
    if (!g_file_load_contents (shader_file, NULL, &shader_src, &src_len,
                               NULL, &err)) {
        GST_ERROR_OBJECT (sink, "Could not read shader source: %s\n",
                         err->message);
        g_free (err);
        g_object_unref (shader_file);
        glDeleteShader (shader);
        return 0;
    }

    /* load source into shader object */
    src_len = strlen (shader_src);
    glShaderSource (shader, 1, (const GLchar**) &shader_src,
                    (const GLint*) &src_len);

    /* shader code has been loaded into GL, free all resources
     * we have used to load the shader */
    g_free (shader_src);
    g_object_unref (shader_file);

    /* compile the shader */
    glCompileShader (shader);

    /* check compiler status */
    glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;

        glGetShaderiv (shader, GL_INFO_LOG_LENGTH, &info_len);
        if(info_len > 1) {
            char *info_log = malloc (sizeof(char) * info_len);
            glGetShaderInfoLog (shader, info_len, NULL, info_log);

            GST_ERROR_OBJECT (sink, "Failed to compile shader: %s", info_log);
            free (info_log);
        }

        glDeleteShader (shader);
        shader = 0;
    } else {
	GST_DEBUG_OBJECT (sink, "Shader compiled succesfully");
    }

    return shader;
}

/*
 * Loads a shader from either precompiled binary file when possible.
 * If no binary is found the source file is taken and compiled at
 * runtime. */
static GLuint
gl_load_shader (GstElement *sink, const gchar *basename, const GLenum type)
{
    GstGLESSink *el = GST_GLES_SINK (sink);
    gchar *filename;
    GLuint shader;

    filename = g_strdup_printf ("%s/%s%s", DATA_DIR, basename,
                                SHADER_EXT_BINARY);
    GST_DEBUG_OBJECT (el, "Load binary shader from %s", filename);

    shader = gl_load_binary_shader (sink, filename, type);
    if (!shader) {
        g_free (filename);
        filename = g_strdup_printf ("%s/%s%s", DATA_DIR,
                                    basename,
                                    SHADER_EXT_SOURCE);
        GST_DEBUG_OBJECT(el, "Load source shader from %s", filename);

        shader = gl_load_source_shader(sink, filename, type);
    }

    g_free (filename);
    return shader;
}

/*
 * Load vertex and fragment Shaders.
 * Vertex shader is a predefined default, fragment shader can be configured
 * through process_type */
static gint
gl_load_shaders (GstElement *sink, GstGLESShader *shader,
                 GstGLESShaderTypes process_type)
{
    shader->vertex_shader = gl_load_shader (sink, VERTEX_SHADER_BASENAME,
                                          GL_VERTEX_SHADER);
    if (!shader->vertex_shader)
        return -EINVAL;

    shader->fragment_shader = gl_load_shader (sink,
                                            shader_basenames[process_type],
                                            GL_FRAGMENT_SHADER);
    if (!shader->fragment_shader)
        return -EINVAL;

    return 0;
}

gint
gl_init_shader (GstElement *sink, GstGLESShader *shader,
                GstGLESShaderTypes process_type)
{
    gint linked;
    GLint err;
    gint ret;

    shader->program = glCreateProgram();
    if(!shader->program) {
        GST_ERROR_OBJECT(sink, "Could not create GL program");
        return -ENOMEM;
    }

    /* load the shaders */
    ret = gl_load_shaders(sink, shader, process_type);
    if(ret < 0) {
        GST_ERROR_OBJECT(sink, "Could not create GL shaders: %d", ret);
        return ret;
    }

    glAttachShader(shader->program, shader->vertex_shader);
    err = glGetError ();
    if (err != GL_NO_ERROR) {
        GST_ERROR_OBJECT (sink, "Error while attaching the vertex shader: 0x%04x\n", err);
    }

    glAttachShader(shader->program, shader->fragment_shader);
    err = glGetError ();
    if (err != GL_NO_ERROR) {
        GST_ERROR_OBJECT (sink, "Error while attaching the fragment shader: 0x%04x\n", err);
    }

    glBindAttribLocation(shader->program, 0, "vPosition");
    glLinkProgram(shader->program);

    /* check linker status */
    glGetProgramiv(shader->program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLint info_len = 0;
        GST_ERROR_OBJECT(sink, "Linker failure");

        glGetProgramiv(shader->program, GL_INFO_LOG_LENGTH, &info_len);
        if(info_len > 1) {
            char *info_log = malloc(sizeof(char) * info_len);
            glGetProgramInfoLog(shader->program, info_len, NULL, info_log);

            GST_ERROR_OBJECT(sink, "Failed to link GL program: %s", info_log);
            free(info_log);
        }

        glDeleteProgram(shader->program);
        return -EINVAL;
    }

    glUseProgram(shader->program);

    shader->position_loc = glGetAttribLocation(shader->program, "vPosition");
    shader->texcoord_loc = glGetAttribLocation(shader->program, "aTexcoord");

    glClearColor(0.0, 0.0, 0.0, 1.0);

    return 0;
}

void
gl_delete_shader(GstGLESShader *shader)
{
    glDeleteShader (shader->vertex_shader);
    shader->vertex_shader = 0;

    glDeleteShader (shader->fragment_shader);
    shader->fragment_shader = 0;

    glDeleteProgram (shader->program);
    shader->program = 0;
}
