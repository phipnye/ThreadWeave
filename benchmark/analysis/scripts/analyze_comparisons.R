# Load and process data ---------------------------------------------------------------------------------
source("setup.R")
json <- read_json(JSONS("comparisons_results.json"))
DT <- rbindlist(json$benchmarks, use.names = TRUE)

# One row per (run, family) with columns for each aggregate stat (mean, median, cv, ...)
stopifnot(all(DT[, time_unit == "ms"]))
stopifnot(DT[, .N, by = .(run_name, family_index, per_family_instance_index, aggregate_name)][, N == 1])
DT <- dcast(
  DT,
  run_name + family_index + per_family_instance_index ~ aggregate_name,
  value.var = "real_time"
)

# Parse metadata out of the benchmark run name, e.g. "BSUnbalanced/4/1000"
DT[, package_name := str_to_upper(str_sub(run_name, 1, 2))]
DT[, balanced := str_extract(run_name, "(?i)u?n?balanced")]
DT[, c("run_name", "n_threads", "n_tasks", "real_time") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT[, c("run_name", "real_time", "family_index", "per_family_instance_index") := NULL]
setcolorder(DT, c("package_name", "balanced", "n_threads", "n_tasks"))
setorder(DT, n_threads, n_tasks, balanced, package_name)
stopifnot(all(DT[, package_name %in% c("TW", "BS")]))
stopifnot(all(DT[, .N, by = .(n_tasks, n_threads, balanced)][, N == 2]))

# Speedup > 1 means TW is faster
DT[, speedup := mean[package_name == "BS"] / mean[package_name == "TW"], by = .(n_tasks, n_threads, balanced)]

# Plot labels
DT[, thread_label := paste(n_threads, "Threads")]
DT[, task_label := factor(
  fifelse(n_tasks >= 1000, paste0(n_tasks / 1000, "k Tasks"), paste(n_tasks, "Tasks")),
  levels = unique(fifelse(
    sort(unique(n_tasks)) >= 1000,
    paste0(sort(unique(n_tasks)) / 1000, "k Tasks"),
    paste(sort(unique(n_tasks)), "Tasks")
  ))
)]
DT[, pairing := paste(package_name, balanced)]

# Plot 1: Direct comparison ratios (TW time / BS time) --------------------------------------------------

ggplot(DT[package_name == "TW"], aes(x = factor(n_tasks), y = speedup, fill = factor(n_threads))) +
  geom_bar(stat = "identity", position = position_dodge(width = 0.8), width = 0.7) +
  geom_text(
    aes(label = sprintf("%.2f", speedup)),
    position = position_dodge(width = 0.8),
    vjust = -0.4,
    size = 3
  ) +
  geom_hline(yintercept = 1, linetype = "dashed", color = "red", linewidth = 0.8) +
  facet_wrap(~balanced, labeller = label_value) +
  labs(
    title = "Performance Speedup",
    subtitle = "Instances Above 1.0 Line Indicate TW Faster",
    x = "Number of Tasks",
    y = "Speedup Ratio",
    fill = "Threads"
  ) +
  theme_bw()

ggsave(PLOTS("comparisons_01_performance_ratios.png"), width = 9, height = 5.5, dpi = 300)

# Plot 2: Execution time trend by thread count and workload type ----------------------------------------

ggplot(DT, aes(x = n_tasks, y = mean / 1000, color = package_name, group = package_name)) +
  geom_line() +
  geom_point() +
  scale_x_log10(labels = scales::comma) +
  scale_y_log10() +
  facet_grid(balanced ~ thread_label, labeller = label_value) +
  labs(
    title = "Execution Time by Thread Count and Workload Type",
    subtitle = "Log-Log Scale",
    x = "Number of Tasks (log scale)",
    y = "Mean Time in Seconds (log scale)",
    color = "Package"
  ) +
  theme_bw()

ggsave(PLOTS("comparisons_02_execution_time_trend.png"), width = 11, height = 6, dpi = 300)

# Plot 3: Library-workload pairings across thread and task count ----------------------------------------

ggplot(DT, aes(x = factor(n_threads), y = mean / 1000, fill = pairing)) +
  geom_bar(stat = "identity", position = position_dodge(width = 0.9), width = 0.8) +
  facet_wrap(~task_label, scale = "free_y") +
  labs(
    title = "Library-Workload Pairings Across Thread Counts",
    x = "Threads",
    y = "Mean Time (Seconds)",
    fill = "Package / Workload"
  ) +
  theme_bw()

ggsave(PLOTS("comparisons_03_library_workload_pairings.png"), width = 10, height = 6, dpi = 300)
