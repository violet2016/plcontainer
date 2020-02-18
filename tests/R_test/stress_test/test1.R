#!/usr/bin/env Rscript
#context("Test matrix of gpapply")

## ----------------------------------------------------------------------
## Test preparations
library(GreenplumR)
# Need valid 'pivotalr_port' and 'pivotalr_dbname' values
env <- new.env(parent = globalenv())
#.dbname = get('pivotalr_dbname', envir=env)
#.port = get('pivotalr_port', envir=env)
.verbose <- TRUE

.host <- Sys.getenv('PGHOST', 'localhost')
.dbname <- "test"
.port <- strtoi(Sys.getenv('PGPORT'))
if (is.na(.port))
    stop("PGPORT not set")
.language <- tolower(Sys.getenv('GPRLANGUAGE'))
if (.language != 'plr' && .language != 'plcontainer')
    stop(paste0("invalid GPRLANGUAGE:", .language))
## connection ID
cid <- db.connect(host = .host, port = .port, dbname = .dbname, verbose = .verbose)


tname.mul.col <- 'num_of_loops'

.signature <- list("num" = "int", "aux" = "int")
fn.function_1 <- function(num)
{
    a <- matrix(rnorm(200), ncol=10, nrow=20)
    b <- matrix(rnorm(200), ncol=20, nrow=10)
    a %*% b
    return (num)
}
fn.function_3 <- function(num)
{
    a <- 0
    #for (i in 1:10000){
    for (i in 1:100){
        a <- a + num
    }
    return (a)
}

fn.function_4 <- function(num)
{
    return (num * 2)
}
fn.function_5 <- function(num)
{
    return (num + 1)
}
.output.name <- NULL

mul <- db.data.frame(tname.mul.col, conn.id= cid, verbose = .verbose)
stopifnot(is.db.data.frame(mul) == TRUE)
print("start funciton 1")
res <- db.gpapply(mul, output.name = .output.name,
                FUN = fn.function_1, output.signature = .signature,
                clear.existing = TRUE, case.sensitive = TRUE, language = .language)
print("start funciton 3")
res <- db.gpapply(mul, output.name = .output.name,
                FUN = fn.function_3, output.signature = .signature,
                clear.existing = TRUE, case.sensitive = TRUE, language = .language)
print("start funciton 4")
res <- db.gpapply(mul, output.name = .output.name,
                FUN = fn.function_4, output.signature = .signature,
                clear.existing = TRUE, case.sensitive = TRUE, language = .language)
print("start function 5")
res <- db.gpapply(mul, output.name = .output.name,
                FUN = fn.function_5, output.signature = .signature,
                clear.existing = TRUE, case.sensitive = TRUE, language = .language)
db.disconnect(cid)