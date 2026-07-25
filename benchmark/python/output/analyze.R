library(ggplot2)
library(jsonlite)
library(data.table)
library(stringr)


# Load and format data ----------------------------------------------------------------------------------

json <- jsonlite::read_json("results.json")
benches <- json$benchmarks
DT <- list2DF(benches)
setDT(DT)
DT <- transpose(DT)
setnames(DT,
         c("name",
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
           "label")
         )

DT[, names(.SD) := lapply(.SD, unlist)]
DT <- dcast(
  DT, 
  run_name + family_index + per_family_instance_index ~ aggregate_name, 
  value.var = "real_time"
)
DT[, package_name := str_to_upper(str_sub(run_name, 1, 2))]
DT[, balanced := str_detect(run_name, "Un[Bb]alanced", negate = TRUE)]
DT[, c("run_name", "n_threads", "n_tasks", "real_time") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT[, c("run_name", "real_time", "family_index", "per_family_instance_index") := NULL]
setcolorder(DT, c("package_name", "balanced", "n_threads", "n_tasks"))
setorder(DT, n_threads, n_tasks, balanced, package_name)

DT2 <- dcast(DT, n_threads + n_tasks + balanced ~ package_name, value.var = "mean")
DT2[, speedup := BS / TW] # <1 means BS is faster


# Plots -------------------------------------------------------------------------------------------------

ggplot(DT, aes(x = factor(n_tasks), y = mean, fill = package_name)) +
  geom_bar(stat = "identity", position = position_dodge(width = 0.8), width = 0.7) +
  facet_grid(balanced ~ n_threads, scales = "free_y", labeller = label_both) +
  scale_y_log10() +
  labs(
    title = "BS vs TW Execution Time Across Configurations",
    subtitle = "Faceted by Workload Balance (Rows) and Thread Count (Columns)",
    x = "Number of Tasks",
    y = "Mean Time (ms, log10 scale)",
    fill = "Package"
  ) +
  theme_minimal(base_size = 13) +
  theme(legend.position = "bottom")

ggplot(DT, aes(x = factor(n_tasks), y = cv, color = package_name, group = interaction(package_name, n_threads))) +
  geom_line(linewidth = 0.8) +
  geom_point(size = 2) +
  facet_grid(balanced ~ n_threads, labeller = label_both) +
  labs(
    title = "Coefficient of Variation (Stability) Comparison",
    subtitle = "Lower CV indicates more consistent/stable run times",
    x = "Number of Tasks",
    y = "Coefficient of Variation (CV)",
    color = "Package"
  ) +
  theme_minimal(base_size = 13) +
  theme(legend.position = "bottom")

ggplot(DT2, aes(x = factor(n_tasks), y = speedup, fill = factor(n_threads))) +
  geom_bar(stat = "identity", position = position_dodge(width = 0.8), width = 0.7) +
  geom_hline(yintercept = 1, linetype = "dashed", color = "red", linewidth = 0.8) +
  facet_wrap(~ balanced, labeller = label_both) +
  labs(
    title = "Performance Speedup Ratio (TW Time / BS Time)",
    subtitle = "Bars below the red dashed line indicate BS is faster; above means TW is faster",
    x = "Number of Tasks",
    y = "Speedup Factor (Multiplier)",
    fill = "Threads"
  ) +
  theme_minimal(base_size = 13) +
  theme(legend.position = "right", panel.grid.minor = element_blank())

ggplot(DT[(balanced)], aes(x = factor(n_tasks), y = mean / 1000, color = package_name, group = package_name)) +
  geom_line(linewidth = 1.2) +
  geom_point(size = 3) +
  facet_wrap(~ n_threads, scales = "free_y", labeller = label_both) +
  labs(
    title = "Execution Time for Balanced Workloads",
    x = "Number of Tasks",
    y = "Mean Time (Seconds)",
    color = "Package"
  ) +
  theme_minimal(base_size = 13) +
  theme(legend.position = "bottom", panel.grid.minor = element_blank())


ggplot(DT[(!balanced)], aes(x = factor(n_tasks), y = mean / 1000, color = package_name, group = package_name)) +
  geom_line(linewidth = 1.2) +
  geom_point(size = 3) +
  facet_wrap(~ n_threads, scales = "free_y", labeller = label_both) +
  labs(
    title = "Execution Time for Unbalanced Workloads",
    x = "Number of Tasks",
    y = "Mean Time (Seconds)",
    color = "Package"
  ) +
  theme_minimal(base_size = 13) +
  theme(legend.position = "bottom", panel.grid.minor = element_blank())
