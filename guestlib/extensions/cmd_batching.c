#include "common/extensions/cmd_batching.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "common/cmd_channel.hpp"
#include "common/debug.hpp"
#include "common/endpoint_lib.hpp"
#include "common/linkage.h"
#include "common/logging.h"
#include "common/shadow_thread_pool.hpp"

#define batch_array_index(array, index_) ((struct command_base *)g_ptr_array_index(array, index_))

struct command_batch *nw_global_cmd_batch = NULL;

struct command_wrapper {
  struct command_base *cmd;
  struct command_channel *chan;
  int is_async;
};

/**
 * batch_emit - Emit batched commands to the API server
 * @active_cmds: the commands to be emitted
 */
static void batch_emit(GAsyncQueue *active_cmds) {
  ava_debug("Emit a batch with %u commands\n", g_async_queue_length(active_cmds));

  GQueue *queue = g_queue_new();
  size_t __total_buffer_size = 0;
  {
    gpointer cmd;
    while ((cmd = g_async_queue_try_pop(active_cmds))) {
      g_queue_push_head(queue, cmd);
      __total_buffer_size += ((struct command_base *)cmd)->command_size + ((struct command_base *)cmd)->region_size;
    }
  }

  void *__command_buffer = malloc(__total_buffer_size);
  {
    off_t offset = 0;
    struct command_base *cmd;
    while ((cmd = (struct command_base *)g_queue_pop_tail(queue))) {
      memcpy((char *)__command_buffer + offset, (void *)cmd, cmd->command_size + cmd->region_size);
      offset += cmd->command_size + cmd->region_size;
      free(cmd);
    }
    assert(offset == __total_buffer_size);
  }

  /* __do_batch_emit is generated by AvA. */
  __do_batch_emit(__command_buffer, __total_buffer_size);

  free(__command_buffer);
  g_queue_free(queue);
}

/**
 * batch_insert_command - Insert a new command to the batch
 * @cmd_batch: command batch struct
 * @cmd: command to be inserted to the batch
 * @chan: channel to emit the command
 * @is_async: whether this command is asynchronous or not
 *
 * When the batched commands reach the maximum batch size, we emit the batch to
 * the API server.
 */
EXPORTED_WEAKLY void batch_insert_command(struct command_batch *cmd_batch, struct command_base *cmd,
                                          struct command_channel *chan, int is_async) {
  struct command_wrapper *wrap = g_malloc(sizeof(struct command_wrapper));
  wrap->cmd = cmd;
  wrap->chan = chan;
  wrap->is_async = is_async;

  ava_debug("Add command (%ld) to pending batched command list\n", cmd->command_id);
  g_async_queue_push(cmd_batch->pending_cmds, (gpointer)wrap);
}

#define CALL_CUDART_OPT_CU_CTX_SET_CURRENT 102

static void *batch_process_thread(void *opaque) {
  struct command_batch *cmd_batch = (struct command_batch *)opaque;
  struct command_wrapper *wrap;
  gdouble elapsed_time;
  GTimer *timer = g_timer_new();
  int64_t thread_id = shadow_thread_id(nw_shadow_thread_pool);

  cmd_batch->running = 1;
  if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL)) {
    perror("pthread_setcancelstate failed\n");
    exit(0);
  }
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL)) {
    perror("pthread_setcanceltype failed\n");
    exit(0);
  }

  fprintf(stderr, "Start batch processing thread\n");
  g_timer_start(timer);

  while (cmd_batch->running) {
    wrap = (struct command_wrapper *)g_async_queue_timeout_pop(cmd_batch->pending_cmds, (BATCH_QUEUE_TIME_OUT_US));

    if (!cmd_batch->running) break;

    /* Special case:
     * cuCtxSetCurrent must be inserted to the batch to set the correct CUDA context for the
     * shadow thread of batch processing thread.
     *
     * Current solution is to set the emitted synchronous command's thread id to the batch
     * processing thread's.
     */

    /* Emit the single synchronous API. */
    if (wrap && !wrap->is_async && g_async_queue_length(cmd_batch->active_cmds) == 0) {
      ava_debug("Emit a synchronous command %ld, thread id %lx->%lx\n", wrap->cmd->command_id, wrap->cmd->thread_id,
                thread_id);
      wrap->cmd->thread_id = thread_id;
      command_channel_send_command(wrap->chan, wrap->cmd);
      g_free(wrap);
      g_timer_start(timer);
      continue;
    }

    if (wrap) g_async_queue_push(cmd_batch->active_cmds, (gpointer)wrap->cmd);

    /* Emit the batch when there is a synchronous API, or we have enough commands in the batch,
     * or it has been a while (10ms) since last emit. */
    g_timer_stop(timer);
    elapsed_time = g_timer_elapsed(timer, NULL);
    if ((wrap && !wrap->is_async) || g_async_queue_length(cmd_batch->active_cmds) >= BATCH_SIZE ||
        elapsed_time >= BATCH_TIME_OUT_US) {
      if (wrap && !wrap->is_async) {
        ava_debug("Emit a batch ending with a synchronous command %ld\n", wrap->cmd->command_id);
      }

      batch_emit(cmd_batch->active_cmds);
      g_timer_start(timer);
    } else {
      g_timer_continue(timer);
    }

    if (wrap) g_free(wrap);
  }

  g_timer_destroy(timer);

  return NULL;
}

/**
 * batch_init_thread - Initialize command batch
 *
 * This function initializes command batch structure, and starts a batch
 * processing thread.
 */
EXPORTED_WEAKLY struct command_batch *cmd_batch_thread_init(void) {
  struct command_batch *cmd_batch = (struct command_batch *)calloc(1, sizeof(struct command_batch));

  // TODO: extend it to multi-threads
  cmd_batch->pending_cmds = g_async_queue_new_full((GDestroyNotify)free);
  cmd_batch->active_cmds = g_async_queue_new_full((GDestroyNotify)free);
  pthread_create(&cmd_batch->process_thread, NULL, &batch_process_thread, (void *)cmd_batch);

  return cmd_batch;
}

EXPORTED_WEAKLY void cmd_batch_thread_fini(struct command_batch *cmd_batch) {
  cmd_batch->running = 0;
  if (pthread_cancel(cmd_batch->process_thread)) perror("pthread_cancel failed\n");
  pthread_join(cmd_batch->process_thread, NULL);
  g_async_queue_unref(cmd_batch->active_cmds);
  // FIXME: g_async_queue_unref: assertion 'queue->waiting_threads == 0' failed
  // g_async_queue_unref(cmd_batch->pending_cmds);
  free(cmd_batch);
}
