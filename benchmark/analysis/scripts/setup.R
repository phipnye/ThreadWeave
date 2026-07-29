suppressPackageStartupMessages({
  library(data.table)
  library(ggplot2)
  library(jsonlite)
  library(scales)
  library(stringr)  
})

JSONS <- function(x = "") {
  file.path("..", "jsons", x)
}

PLOTS <- function(x = "") {
  file.path("..", "plots", x)
}
