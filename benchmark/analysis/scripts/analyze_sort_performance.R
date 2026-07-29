# Load and process data ---------------------------------------------------------------------------------
source("setup.R")
json <- read_json(JSONS("sort_performance_results.json"))
DT <- rbindlist(json$benchmarks, use.names = TRUE)

# One row per (run, family) with columns for each aggregate stat (mean, median, cv, ...)
stopifnot(all(DT[, time_unit == "ms"]))
stopifnot(DT[, .N, by = .(run_name, family_index, per_family_instance_index, label, aggregate_name)][, N == 1])
DT <- dcast(
  DT,
  run_name + family_index + per_family_instance_index + label ~ aggregate_name,
  value.var = "real_time"
)
DT[, library := str_extract(label, "(?<=library=)[^;]+")]
DT[, c("run_name", "size", "real_time") := tstrsplit(run_name, "/", type.convert = TRUE)]
DT[, c("run_name", "label", "family_index", "per_family_instance_index", "real_time") := NULL]
setcolorder(DT, c("library", "size"))
setorder(DT, library, size)

# Compute speedup relative to the sequential baseline
DT[, speedup := mean[library == "std::sort_sequential"] / mean, by = size]

# Plot labels
DT[, size_lab := factor(
  size,
  levels = sort(unique(size)),
  labels = paste0(format(sort(unique(size)), big.mark = ","), " elements")
)]
DT[, library := factor(
  library,
  levels = c("std::sort_sequential", "std::execution::par", "oneTBB", "ThreadWeave")
)]

# Plot 1: Runtime by library, faceted by vector size ----------------------------------------------------

ggplot(DT, aes(x = library, y = mean, fill = library)) +
  geom_col() +
  geom_text(aes(label = comma(round(mean))), vjust = -0.4, size = 3) +
  facet_wrap(~size_lab, scales = "free_y") +
  scale_y_continuous(labels = comma, expand = expansion(mult = c(0, 0.15))) +
  labs(
    title = "Sort Performance by Library",
    subtitle = "Parallel Performance using Four Threads",
    x = NULL,
    y = "Time (ms)",
    fill = NULL
  ) +
  theme_bw() +
  theme(axis.text.x = element_text(angle = 45, hjust = 1)) +
  guides(fill = "none")

ggsave(
  PLOTS("sort_01_runtime_bars.png"),
  width = 3.5 * length(unique(DT[, size])),
  height = 4.5,
  dpi = 300
)

# Plot 2: Speedup vs. vector size, one line per library -------------------------------------------------

ggplot(DT[library != "std::sort_sequential"], aes(x = size, y = speedup, color = library)) +
  geom_hline(yintercept = 1, linetype = "dashed", color = "grey60") +
  geom_line() +
  geom_point() +
  scale_x_log10(labels = comma, breaks = sort(unique(DT$size))) +
  scale_y_continuous(labels = function(x) paste0(x, "x")) +
  labs(
    title = "Speedup vs. Sequential std::sort, by Vector Size",
    x = "Vector Size (elements)",
    y = "Speedup (relative to std::sort_sequential)",
    color = NULL
  ) +
  theme_bw()

ggsave(
  PLOTS("sort_02_speedup_curves.png"),
  width = 7,
  height = 5,
  dpi = 300
)
