#include "camera.h"
#include "loader.h"
#include "log.h"
#include "mat.h"
#include "model.h"
#include "object.h"
#include "sphere.h"
#include "worker.h"

static void* loader_run(void* loader_);

void loader_wait(loader_t* loader, loader_state_t target) {
    platform_mutex_lock(&loader->mutex);
    while (loader->state < target) {
        platform_cond_wait(&loader->cond, &loader->mutex);
    }
    platform_mutex_unlock(&loader->mutex);
}

void loader_next(loader_t* loader, loader_state_t target) {
    platform_mutex_lock(&loader->mutex);
    loader->state = target;
    platform_cond_broadcast(&loader->cond);
    platform_mutex_unlock(&loader->mutex);
}

loader_t* loader_new(const char* filename) {
    OBJECT_ALLOC(loader);
    platform_mutex_init(&loader->mutex);
    platform_cond_init(&loader->cond);

    loader->filename = filename;

    if (platform_thread_create(&loader->thread, loader_run, loader)) {
        log_error_and_abort("Error creating loader thread");
    }
    return loader;
}

static void* loader_run(void* loader_) {
    loader_t* loader = (loader_t*)loader_;
    loader->buffer = NULL;
    loader_next(loader, LOADER_START);

    /*  Filesize in bytes; needed to munmap file at the end */
    size_t size;

    /*  This magic filename tells us to load a builtin array,
     *  rather than something in the filesystem */
    bool builtin_sphere = !strcmp(loader->filename, ":/sphere");
    const char* mapped;
    if (builtin_sphere) {
        mapped = (const char*)sphere_stl;
        size = sphere_stl_len;
    } else {
        mapped = platform_mmap(loader->filename, &size);
    }

    /*  Check that the file was opened. */
    if (!mapped) {
        log_error("Could not open %s", loader->filename);
        loader_next(loader, LOADER_ERROR_NO_FILE);
        return NULL;
    }

    /*  Check whether this is an ASCII stl */
    if (size >= 6 && !strncmp("solid ", mapped, 6)) {
        loader_next(loader, LOADER_ERROR_ASCII_STL);
        return NULL;
    }

    /*  Check whether the file is a valid size. */
    if (size < 84) {
        log_error("File is too small to be an STL (%i < 84)", size);
        loader_next(loader, LOADER_ERROR_WRONG_SIZE);
        return NULL;
    }

    memcpy(&loader->num_triangles, &mapped[80],
           sizeof(loader->num_triangles));

    /*  Compare the actual file size with the expected size */
    const uint32_t expected_size = loader->num_triangles * 50 + 84;
    if (expected_size != size) {
        log_error("Invalid file size for %u triangles (expected %u, got %u)",
                  loader->num_triangles, expected_size, size);
        loader_next(loader, LOADER_ERROR_WRONG_SIZE);
        return NULL;
    }

    /*  Inform the main thread that it can now create an OpenGL buffer
     *  for the given number of triangles. */
    loader_next(loader, LOADER_TRIANGLE_COUNT);
    float* ram = (float*)malloc(loader->num_triangles * 3 * 3 * sizeof(float));
    loader_next(loader, LOADER_RAM_BUFFER);

    /*  We kick off our mmap-to-OpenGL copying workers here, even though the
     *  buffer won't be ready for a little while.  This means that as soon
     *  as the buffer is ready, they'll start! */
    const size_t NUM_WORKERS = 8;
    worker_t workers[NUM_WORKERS];
    for (unsigned i=0; i < NUM_WORKERS; ++i) {
        const size_t start = i * loader->num_triangles / NUM_WORKERS;
        const size_t end = (i + 1) * loader->num_triangles / NUM_WORKERS;

        workers[i].loader = loader;
        workers[i].count = end - start;
        workers[i].stl = (const char (*)[50])&mapped[80 + 4 + 12 + 50 * start];
        workers[i].ram = (float (*)[9])&ram[start * 9];
        workers[i].gpu = NULL;

        worker_start(&workers[i]);
    }

    log_trace("Waiting for buffer...");
    loader_wait(loader, LOADER_GPU_BUFFER);

    /*  Populate GPU pointers, then kick off workers copying triangles */
    for (unsigned i=0; i < NUM_WORKERS; ++i) {
        const size_t start = i * loader->num_triangles / NUM_WORKERS;
        workers[i].gpu = (float (*)[9])&loader->buffer[start * 9];
    }
    loader_next(loader, LOADER_WORKER_GPU);
    log_trace("Sent buffers to worker threads");

    for (unsigned i=0; i < NUM_WORKERS; ++i) {
        if (platform_thread_join(&workers[i].thread)) {
            log_error_and_abort("Error joining worker thread");
        }
    }
    log_trace("Joined worker threads");

    /*  Reduce min / max arrays from worker subprocesses */
    float scale = 0.0f;
    float center[3];
    for (unsigned v=0; v < 3; ++v) {
        for (unsigned i=1; i < NUM_WORKERS; ++i) {
            if (workers[i].max[v] > workers[0].max[v]) {
                workers[0].max[v] = workers[i].max[v];
            }
            if (workers[i].min[v] < workers[0].min[v]) {
                workers[0].min[v] = workers[i].min[v];
            }
        }
        center[v] = (workers[0].max[v] + workers[0].min[v]) / 2.0f;
        const float d = workers[0].max[v] - workers[0].min[v];
        if (d > scale) {
            scale = d;
        }
    }
    /*  Build the model matrix, which positions the model at
     *  [0,0,0] and scales it to fit the standard GL view */
    float t[4][4];
    mat4_translation(center, t);
    float s[4][4];
    mat4_scaling(1.0f / scale, s);
    mat4_mul(t, s, loader->mat);

    /*  Mark the load as done and post an empty event, to make sure that
     *  the main loop wakes up and checks the loader */
    log_trace("Loader thread done");
    loader_next(loader, LOADER_DONE);
    glfwPostEmptyEvent();

    if (!builtin_sphere) {
        platform_munmap(mapped, size);
    }
    free(ram);

    return NULL;
}

void loader_allocate_vbo(loader_t* loader) {
    glGenBuffers(1, &loader->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, loader->vbo);
    loader_wait(loader, LOADER_TRIANGLE_COUNT);

    /*  Early return if there is an error in the loader;
     *  we leave the buffer allocated so it can be cleaned
     *  up as usual later. */
    if (loader->state >= LOADER_ERROR) {
        return;
    }

    glBufferData(GL_ARRAY_BUFFER, loader->num_triangles * 36,
                 NULL, GL_STATIC_DRAW);
    loader->buffer = (float*)glMapBufferRange(
            GL_ARRAY_BUFFER, 0, loader->num_triangles * 36,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT
                             | GL_MAP_INVALIDATE_BUFFER_BIT
                             | GL_MAP_UNSYNCHRONIZED_BIT);
    loader_next(loader, LOADER_GPU_BUFFER);

    log_trace("Allocated buffer");
}

void loader_finish(loader_t* loader, model_t* model, camera_t* camera) {
    if (!loader->vbo) {
        log_error_and_abort("Invalid loader VBO");
    } else if (!model->vao) {
        log_error_and_abort("Invalid model VAO");
    }

    glBindBuffer(GL_ARRAY_BUFFER, loader->vbo);
    loader_wait(loader, LOADER_DONE);

    /*  If the loader succeeded, then set up all of the
     *  GL buffers, matrices, etc. */
    if (loader->state == LOADER_DONE) {
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindVertexArray(model->vao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

        model->vbo = loader->vbo;
        model->num_triangles = loader->num_triangles;
        loader->vbo = 0;

        memcpy(camera->model, loader->mat, sizeof(loader->mat));
        log_trace("Copied model from loader");
    } else {
        log_error("Loading failed");
    }
}

void loader_delete(loader_t* loader) {
    if (platform_thread_join(&loader->thread)) {
        log_error_and_abort("Failed to join loader thread");
    }
    platform_mutex_destroy(&loader->mutex);
    platform_cond_destroy(&loader->cond);
    free(loader);
    log_trace("Destroyed loader");
}

const char* loader_error_string(loader_state_t state) {
    switch(state) {
        case LOADER_START:
        case LOADER_TRIANGLE_COUNT:
        case LOADER_RAM_BUFFER:
        case LOADER_GPU_BUFFER:
        case LOADER_WORKER_GPU:
        case LOADER_DONE:
            return "No error";

        case LOADER_ERROR:
            return "Generic error";
        case LOADER_ERROR_NO_FILE:
            return "File not found";
        case LOADER_ERROR_ASCII_STL:
            return "ASCII STLs are unsupported";
        case LOADER_ERROR_WRONG_SIZE:
            return "File size does not match triangle count";
    }
}
