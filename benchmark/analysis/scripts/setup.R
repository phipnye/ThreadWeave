ANALYSIS <- function(...) {
  script_dir <- if (!is.null(sys.frames()[[1]]$ofile)) {
    dirname(sys.frames()[[1]]$ofile)
  } else {
    getwd()
  }
  
  normalizePath(file.path(script_dir, "..", ...), mustWork = FALSE)
}

if (!requireNamespace("renv", quietly = TRUE)) {
  install.packages("renv", repos = "https://cloud.r-project.org/")
}

options(renv.verbose = FALSE)
renv::restore(lockfile = ANALYSIS("renv.lock"), prompt = FALSE)
options(renv.verbose = TRUE)

suppressPackageStartupMessages({
library(jsonlite)
library(data.table)
library(ggplot2)
library(stringr)
library(scales)
})

JSONS <- function(file) ANALYSIS("jsons", file)
PLOTS <- function(file) ANALYSIS("plots", file)
