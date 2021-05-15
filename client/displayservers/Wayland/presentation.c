/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2021 Guanzhong Chen (quantum2048@gmail.com)
https://looking-glass.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define _GNU_SOURCE
#include "wayland.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"
#include "common/time.h"

static void presentationClockId(void * data,
    struct wp_presentation * presentation, uint32_t clkId)
{
  app_updateClockId(clkId);
}

static const struct wp_presentation_listener presentationListener = {
  .clock_id = presentationClockId,
};

static void presentationFeedbackSyncOutput(void * data,
    struct wp_presentation_feedback * feedback, struct wl_output * output)
{
  // Do nothing.
}

static void presentationFeedbackPresented(void * opaque,
    struct wp_presentation_feedback * feedback, uint32_t tvSecHi, uint32_t tvSecLo,
    uint32_t tvNsec, uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags)
{
  struct FrameTimes * timings = opaque;
  timings->photon.tv_sec = (time_t) tvSecHi << 32 | tvSecLo;
  timings->photon.tv_nsec = tvNsec;

  struct timespec delta, import, render, photon;
  tsDiff(&delta, &timings->photon, &timings->received);
  tsDiff(&photon, &timings->photon, &timings->swapped);
  tsDiff(&render, &timings->swapped, &timings->imported);
  tsDiff(&import, &timings->imported, &timings->received);

  printf("Presented in %3jd.%06lums since reception, import:%3jd.%06lums, render:%3jd.%06lums, photon:%3jd.%06lums\n",
      (intmax_t) delta.tv_sec * 1000 + delta.tv_nsec / 1000000, delta.tv_nsec % 1000000,
      (intmax_t) import.tv_sec * 1000 + import.tv_nsec / 1000000, import.tv_nsec % 1000000,
      (intmax_t) render.tv_sec * 1000 + render.tv_nsec / 1000000, render.tv_nsec % 1000000,
      (intmax_t) photon.tv_sec * 1000 + photon.tv_nsec / 1000000, photon.tv_nsec % 1000000
  );
  free(timings);
}

static void presentationFeedbackDiscarded(void * opaque,
    struct wp_presentation_feedback * feedback)
{
  free(opaque);
}

static const struct wp_presentation_feedback_listener presentationFeedbackListener = {
  .sync_output = presentationFeedbackSyncOutput,
  .presented = presentationFeedbackPresented,
  .discarded = presentationFeedbackDiscarded,
};

bool waylandPresentationInit(void)
{
  if (wlWm.presentation)
    wp_presentation_add_listener(wlWm.presentation, &presentationListener, NULL);
  return true;
}

void waylandPresentationFree(void)
{
  wp_presentation_destroy(wlWm.presentation);
}

void waylandPresentationFrame(struct FrameTimes * timings)
{
  clock_gettime(app_getClockId(), &timings->swapped);
  struct wp_presentation_feedback * feedback = wp_presentation_feedback(wlWm.presentation, wlWm.surface);
  wp_presentation_feedback_add_listener(feedback, &presentationFeedbackListener, timings);
}
