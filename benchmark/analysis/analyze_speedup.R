# Setup -------------------------------------------------------------------------------------------------

library(ggplot2)
library(jsonlite)
library(data.table)
library(rstudioapi)
library(stringr)
setwd(dirname(rstudioapi::getActiveDocumentContext()$path))

# Load and process data ---------------------------------------------------------------------------------

json <- read_json("output/speedup_results.json")
DT <- list2DF(json$benchmarks)
setDT(DT)
DT <- transpose(DT)
setnames(
  DT,
  c(
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
  )
)
DT[, names(.SD) := lapply(.SD, unlist)]

# One row per (run, family) with columns for each aggregate stat (mean, median, cv, ...)
stopifnot(all(DT[, time_unit == "ms"]))
DT <- dcast(
  DT,
  run_name + family_index + per_family_instance_index ~ aggregate_name,
  value.var = "real_time"
)
DT[, balanced := str_extract(run_name, "(?i)u?n?balanced")]
DT[, c("run_name", "n_threads", "n_tasks", "real_time") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT[, c("run_name", "real_time", "family_index", "per_family_instance_index") := NULL]
setcolorder(DT, c("balanced", "n_threads", "n_tasks"))
setorder(DT, n_threads, n_tasks, balanced)

# Compute speedups and efficiency of adding additional threads
DT[, speedup := mean[n_threads == 1] / mean, by = .(balanced, n_tasks)]
DT[, efficiency := speedup / n_threads]

# Plot labels
DT[, n_tasks_lab := factor(
  n_tasks,
  levels = sort(unique(n_tasks)),
  labels = paste0(format(sort(unique(n_tasks)), big.mark = ","), " tasks")
)]

# Plot 1: Speedup vs threads, faceted by task count, with ideal-linear reference ------------------------

thread_range <- range(DT$n_threads)

ggplot(DT, aes(x = n_threads, y = speedup, color = balanced)) +
  geom_abline(slope = 1, intercept = 0, linetype = "dashed", color = "grey60") +
  geom_line() +
  geom_point() +
  facet_wrap(~n_tasks_lab) +
  scale_x_continuous(breaks = sort(unique(DT$n_threads)), limits = thread_range) +
  scale_y_continuous(breaks = sort(unique(DT$n_threads)), limits = thread_range) +
  coord_fixed(ratio = 1) +
  labs(
    title = "Speedup vs. Number of Threads",
    subtitle = "Dashed line = ideal linear speedup",
    x = "Threads",
    y = "Speedup (relative to 1 thread)",
    color = NULL
  ) +
  theme_bw()

ggsave(
  "output/speedup_01_speedup_curves.png",
  width = 3.5 * length(unique(DT[, n_tasks])),
  height = 4.5,
  dpi = 300
)

#  Plot 2: Parallel efficiency vs threads (speedup / n_threads) -----------------------------------------

ggplot(DT, aes(x = n_threads, y = efficiency, color = balanced)) +
  geom_hline(yintercept = 1, linetype = "dashed", color = "grey60") +
  geom_line() +
  geom_point() +
  facet_wrap(~n_tasks_lab) +
  scale_x_continuous(breaks = sort(unique(DT$n_threads))) +
  scale_y_continuous(labels = scales::percent, limits = c(0, NA)) +
  labs(
    title = "Parallel Efficiency vs. Number of Threads",
    subtitle = "Efficiency = speedup / threads (100% = perfect scaling)",
    x = "Threads",
    y = "Efficiency",
    color = NULL
  ) +
  theme_bw()

ggsave(
  "output/speedup_02_efficiency_curves.png",
  width = 3.5 * length(unique(DT[, n_tasks])),
  height = 4.5,
  dpi = 300
)
