# Load and process data ---------------------------------------------------------------------------------
source("setup.R")
json <- read_json(JSONS("latency_results.json"))
DT <- rbindlist(json$benchmarks, use.names = TRUE, fill = TRUE)

# One row per (run, family) with columns for each aggregate stat (mean, median, cv, ...)
stopifnot(all(DT[, time_unit == "us"]))
stopifnot(DT[, .N, by = .(run_name, family_index, per_family_instance_index, aggregate_name)][, N == 1])
DT <- dcast(
  DT,
  run_name + family_index + per_family_instance_index ~ aggregate_name,
  value.var = "real_time"
)

# The two benchmark functions embed their identity in run_name
DT[, benchmark_type := fifelse(
  str_detect(run_name, "SingleTaskOverhead"),
  "single_latency",
  "batch_throughput"
)]

# Single-task latency: varies by thread count only
DT_single <- DT[benchmark_type == "single_latency"]
DT_single[, c("run_name", "n_threads", "real_time") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT_single[, c("run_name", "family_index", "per_family_instance_index", "benchmark_type", "real_time") := NULL]
setorder(DT_single, n_threads)

# Batch throughput: varies by thread count and task count
DT_batch <- DT[benchmark_type == "batch_throughput"]
DT_batch[, c("run_name", "n_threads", "n_tasks", "real_time") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT_batch[, c("run_name", "family_index", "per_family_instance_index", "benchmark_type", "real_time") := NULL]
setorder(DT_batch, n_threads, n_tasks)

# Plot labels
DT_batch[, task_label := factor(
  fifelse(n_tasks >= 1000, paste0(n_tasks / 1000, "k Tasks"), paste(n_tasks, "Tasks")),
  levels = unique(fifelse(
    sort(unique(n_tasks)) >= 1000,
    paste0(sort(unique(n_tasks)) / 1000, "k Tasks"),
    paste(sort(unique(n_tasks)), "Tasks")
  ))
)]

# Plot 1: Single-task submission/scheduling/retrieval latency by thread count ---------------------------

ggplot(DT_single, aes(x = factor(n_threads), y = mean)) +
  geom_col(fill = "#3B7EA1", width = 0.55, alpha = 0.9) +
  geom_errorbar(
    aes(ymin = mean - stddev, ymax = mean + stddev),
    width = 0.18,
    linewidth = 0.6,
    color = "grey30"
  ) +
  geom_text(
    aes(y = mean + stddev, label = sprintf("%.1f \u00b5s", mean)),
    vjust = -0.8,
    size = 3.8,
    fontface = "bold",
    color = "grey20"
  ) +
  scale_y_continuous(expand = expansion(mult = c(0, 0.15))) +
  labs(
    title = "Single-Task Latency by Thread Count",
    subtitle = "Time to submit, schedule, and retrieve one task",
    caption = "Error bars represent \u00b11 standard deviation",
    x = "Threads",
    y = "Mean Latency (\u00b5s)"
  ) +
  theme_bw()

ggsave(PLOTS("latency_01_single_task_overhead.png"), width = 7, height = 5, dpi = 300)

# Plot 2: Amortized per-task time under batch load, by thread count and task count -----------------------

ggplot(DT_batch, aes(x = factor(n_threads), y = mean / n_tasks, color = task_label, group = task_label)) +
  geom_line(linewidth = 1) +
  geom_point(size = 2.2) +
  labs(
    title = "Amortized Per-Task Time Under Batch Load",
    subtitle = "Mean batch time divided by number of tasks submitted",
    x = "Threads",
    y = "Amortized Time per Task (\u00b5s)",
    color = "Batch Size"
  ) +
  theme_bw()

ggsave(PLOTS("latency_02_batch_amortized_time.png"), width = 9, height = 5.5, dpi = 300)
