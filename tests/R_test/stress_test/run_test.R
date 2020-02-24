#!/usr/bin/env Rscript
library(parallel)
source("test1.R")
args = commandArgs(trailingOnly=TRUE)
print(length(args))
if (length(args) < 1) {
    stop("Please set iteration number ", call.=FALSE)
}
iter = args[1]

print(paste("run test with", iter, "iters"))
system.time(stressTest(iter))