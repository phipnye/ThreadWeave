# Setup -------------------------------------------------------------------------------------------------

library(ggplot2)
library(jsonlite)
library(data.table)
library(rstudioapi)
library(stringr)
setwd(dirname(rstudioapi::getActiveDocumentContext()$path))

# Load and process data ---------------------------------------------------------------------------------

json <- read_json("output/latency_results.json")
DT <- list2DF(json$benchmarks)
setDT(DT)
DT <- transpose(DT)
setnames(DT, c(
  "name",
  "family_index",
  "per_family_instance_index",
  "run_name",
  "run_type",
  "repetitions",
  "threads",
  "aggregate_name",
  "aggregate_unit",
  "iterations",
  "real_time",
  "cpu_time",
  "time_unit",
  "label"
))
DT[, names(.SD) := lapply(.SD, unlist)]
stopifnot(all(DT[, time_unit == "us"]))

# One row per (run, family) with columns for each aggregate stat (mean, median, cv, ...)
DT <- dcast(
  DT,
  run_name + family_index + per_family_instance_index ~ aggregate_name,
  value.var = "real_time"
)

# The two benchmark functions embed their identity in run_name, e.g.
#   "twLatencySingleTaskOverheadBM/1"          (single-arg: threads only)
#   "twLatencyBatchThroughputBM/4/10000"       (two-arg: threads, tasks)
DT[, benchmark_type := fifelse(
  str_detect(run_name, "SingleTaskOverhead"),
  "single_latency",
  "batch_throughput"
)]

# Single-task latency: varies by thread count only ------------------------------------------------------
DT_single <- DT[benchmark_type == "single_latency"]
DT_single[, c("run_name", "n_threads") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT_single[, c("run_name", "family_index", "per_family_instance_index", "benchmark_type") := NULL]
setorder(DT_single, n_threads)

# Batch throughput: varies by thread count and task count -----------------------------------------------
DT_batch <- DT[benchmark_type == "batch_throughput"]
DT_batch[, c("run_name", "n_threads", "n_tasks") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT_batch[, c("run_name", "family_index", "per_family_instance_index", "benchmark_type") := NULL]
setorder(DT_batch, n_threads, n_tasks)

# Plot labels
DT_batch[, task_label := factor(
  ifelse(n_tasks >= 1000, paste0(n_tasks / 1000, "k Tasks"), paste(n_tasks, "Tasks")),
  levels = unique(fifelse(
    sort(unique(n_tasks)) >= 1000,
    paste0(sort(unique(n_tasks)) / 1000, "k Tasks"),
    paste(sort(unique(n_tasks)), "Tasks")
  ))
)]

# Plot 1: Single-task submission/scheduling/retrieval latency by thread count ---------------------------

ggplot(DT_single, aes(x = factor(n_threads), y = mean)) +
  geom_col(fill = "#2C7FB8", width = 0.6) +
  geom_errorbar(aes(ymin = mean - stddev, ymax = mean + stddev), width = 0.15) +
  geom_text(aes(label = sprintf("%.1f", mean)), vjust = -1.6, size = 3.5) +
  labs(
    title = "Single-Task Latency by Thread Count",
    subtitle = "Time to submit, schedule, and retrieve one task (error bars = ±1 SD)",
    x = "Threads",
    y = "Mean Latency (\u00b5s)"
  ) +
  theme_bw()

ggsave("output/latency_01_single_task_overhead.png", width = 7, height = 5, dpi = 300)

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

ggsave("output/latency_02_batch_amortized_time.png", width = 9, height = 5.5, dpi = 300)