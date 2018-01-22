/* Compiling:
 *
 *   gcc sdl2.c -o ./sdl2 -O2 \
 *       $(pkg-config --cflags --libs sdl2 SDL2_image vulkan libplacebo)
 *
 * Notes:
 *
 * - This proof-of-concept is extremely naive. It uses global state, and
 *   ignores uninitialization on errors (just exit()s). This is probably not
 *   what you should be doing for a real program, but I wanted to avoid the
 *   example becoming too complicated.
 *
 * License: CC0 / Public Domain
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <SDL2/SDL_image.h>
#include <vulkan/vulkan.h>

#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480

const struct pl_render_params render_params = {
    // due to current limitations
    .upscaler = &pl_filter_ewa_lanczos,
    .downscaler = NULL,

    .deband_params = &pl_deband_default_params,
    .dither_params = &pl_dither_default_params,
};

SDL_Window *window;
VkSurfaceKHR surf;
struct pl_context *ctx;

const struct pl_vulkan *vk;
const struct pl_vk_inst *vk_inst;
const struct ra_swapchain *swapchain;

// for rendering
struct pl_plane plane;
struct pl_renderer *renderer;

static void init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
        exit(1);
    }

    window = SDL_CreateWindow("libplacebo demo",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              WINDOW_WIDTH, WINDOW_HEIGHT,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                              SDL_WINDOW_VULKAN);

    if (!window) {
        fprintf(stderr, "Failed creating window: %s\n", SDL_GetError());
        exit(1);
    }
}

static void init_placebo() {
    ctx = pl_context_create(PL_API_VER, &(struct pl_context_params) {
        .log_cb    = pl_log_color,
        .log_level = PL_LOG_DEBUG,
    });

    assert(ctx);
}

static void init_vulkan()
{
    struct pl_vk_inst_params iparams = pl_vk_inst_default_params;
    iparams.debug = true;

    unsigned int num = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &num, NULL)) {
        fprintf(stderr, "Failed enumerating vulkan extensions: %s\n", SDL_GetError());
        exit(1);
    }

    iparams.extensions = malloc(num * sizeof(const char *));
    iparams.num_extensions = num;
    assert(iparams.extensions);

    bool ok = SDL_Vulkan_GetInstanceExtensions(window, &num, iparams.extensions);
    assert(ok);

    if (num > 0) {
        printf("Requesting %d additional vulkan extensions:\n", num);
        for (int i = 0; i < num; i++)
            printf("    %s\n", iparams.extensions[i]);
    }

    vk_inst = pl_vk_inst_create(ctx, &iparams);
    if (!vk_inst) {
        fprintf(stderr, "Failed creating vulkan instance!");
        exit(2);
    }

    free(iparams.extensions);
    if (!SDL_Vulkan_CreateSurface(window, vk_inst->instance, &surf)) {
        fprintf(stderr, "Failed creating vulkan surface: %s\n", SDL_GetError());
        exit(1);
    }

    struct pl_vulkan_params params = pl_vulkan_default_params;
    params.instance = vk_inst->instance;
    params.surface = surf;
    params.allow_software = true;
    vk = pl_vulkan_create(ctx, &params);
    if (!vk) {
        fprintf(stderr, "Failed creating vulkan device!");
        exit(2);
    }

    swapchain = pl_vulkan_create_swapchain(vk, &(struct pl_vulkan_swapchain_params) {
        .surface = surf,
        .present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR,
    });

    if (!swapchain) {
        fprintf(stderr, "Failed creating vulkan swapchain!");
        exit(2);
    }
}

static void init_rendering(const char *filename)
{
    SDL_Surface *img = IMG_Load(filename);
    if (!img) {
        fprintf(stderr, "Failed loading '%s': %s\n", filename, SDL_GetError());
        exit(1);
    }

    const SDL_PixelFormat *fmt = img->format;
    if (SDL_ISPIXELFORMAT_INDEXED(fmt->format) || fmt->BytesPerPixel == 3) {
        // Work-around for real-world GPU limitations.
        // FIXME: Get rid of this once libplacebo supports built-in conversions!
        SDL_Surface *fixed;
        fixed = SDL_CreateRGBSurfaceWithFormat(0, img->w, img->h, 32,
                                               SDL_PIXELFORMAT_ABGR8888);
        SDL_BlitSurface(img, NULL, fixed, NULL);
        SDL_FreeSurface(img);
        img = fixed;
        fmt = img->format;
    }

    struct pl_plane_data data = {
        .type = RA_FMT_UNORM,
        .width = img->w,
        .height = img->h,
        .pixel_stride = fmt->BytesPerPixel,
        .row_stride = img->pitch,
        .pixels = img->pixels,
    };

    uint64_t masks[4] = { fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask };
    pl_plane_data_from_mask(&data, masks);

    bool ok = pl_upload_plane(vk->ra, &plane, &data);
    SDL_FreeSurface(img);

    if (!ok) {
        fprintf(stderr, "Failed uploading texture to GPU!\n");
        exit(2);
    }

    // Create a renderer instance
    renderer = pl_renderer_create(ctx, vk->ra);
}

static void render_frame(const struct ra_swapchain_frame *frame)
{
    const struct ra_tex *img = plane.texture;
    struct pl_image image = {
        .num_planes = 1,
        .planes = { plane },
        .repr = pl_color_repr_unknown,
        .color = pl_color_space_unknown,
        .width = img->params.w,
        .height = img->params.h,
        .src_rect = { 0, 0, img->params.w, img->params.h },
    };

    struct pl_render_target target;
    pl_render_target_from_swapchain(&target, frame);
    if (!pl_render_image(renderer, &image, &target, &render_params))
        fprintf(stderr, "Failed rendering frame!\n");
}

static void uninit()
{
    pl_renderer_destroy(&renderer);
    ra_tex_destroy(vk->ra, &plane.texture);
    ra_swapchain_destroy(&swapchain);
    pl_vulkan_destroy(&vk);
    vkDestroySurfaceKHR(vk_inst->instance, surf, NULL);
    pl_vk_inst_destroy(&vk_inst);
    pl_context_destroy(&ctx);

    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(int argc, const char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: ./sdl2 <filename>\n");
        return 255;
    }

    unsigned int start = SDL_GetTicks();

    int ret = 0;
    init_sdl();
    init_placebo();
    init_vulkan();
    init_rendering(argv[1]);

    unsigned int last = SDL_GetTicks(), frames = 0;
    printf("Took %u ms for initialization\n", last - start);

    while (true) {
        SDL_Event evt;
        while (SDL_PollEvent(&evt) == 1) {
            if (evt.type == SDL_QUIT)
                goto cleanup;
        }

        struct ra_swapchain_frame frame;
        bool ok = ra_swapchain_start_frame(swapchain, &frame);
        if (!ok) {
            SDL_Delay(10);
            continue;
        }

        render_frame(&frame);
        ok = ra_swapchain_submit_frame(swapchain);
        if (!ok) {
            fprintf(stderr, "Failed submitting frame!");
            ret = 3;
            goto cleanup;
        }

        ra_swapchain_swap_buffers(swapchain);
        frames++;

        unsigned int now = SDL_GetTicks();
        if (now - last > 5000) {
            printf("%u frames in %u ms = %f FPS\n", frames, now - last,
                   1000.0f * frames / (now - last));
            last = now;
            frames = 0;
        }
    }

cleanup:
    uninit();
    return ret;
}