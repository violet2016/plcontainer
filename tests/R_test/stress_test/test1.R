#!/usr/bin/env Rscript

library(GreenplumR)

fn.function_1 <- function(num)
{
    a <- matrix(rnorm(200), ncol=10, nrow=20)
    b <- matrix(rnorm(200), ncol=20, nrow=10)
    a %*% b
    return (num)
}
fn.function_3 <- function(num)
{
    a <- 0.0
    #for (i in 1:10000){
    for (i in 1:100) {
        a <- 0.0
        for (i in 1:100){
            a <- a+num[[1]]
        }
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
stressTest <- function(loop) {
    env <- new.env(parent = globalenv())
    .verbose <- TRUE

    .host <- Sys.getenv('PGHOST', 'localhost')
    .dbname <- "test"
    .port <- strtoi(Sys.getenv('PGPORT', 5432))
    if (is.na(.port))
        stop("PGPORT not set")
    .language <- tolower(Sys.getenv('GPRLANGUAGE'))
    if (.language != 'plr' && .language != 'plcontainer')
        stop(paste0("invalid GPRLANGUAGE:", .language))
    ## connection ID
    cid <- db.connect(host = .host, port = .port, dbname = .dbname, verbose = .verbose)


    tname.mul <- 'num_of_loops'

    .signature <- list("num" = "int", "aux" = "int")
    .output.name <- NULL

    mul <- db.data.frame(tname.mul, conn.id= cid, verbose = .verbose)
    stopifnot(is.db.data.frame(mul) == TRUE)
    for (l in 1:loop) {
        print(paste("Start Loop", l))
        res <- db.gpapply(mul, output.name = .output.name,
                        FUN = fn.function_1, output.signature = .signature,
                        clear.existing = TRUE, case.sensitive = TRUE, language = .language)

        z1 <- as.db.data.frame(mul$num, field.types = list(num="integer"))
        .sig3 <- list("num" = "int")
        res <- db.gpapply(z1, output.name = .output.name,
                        FUN = fn.function_3, output.signature = .sig3,
                        clear.existing = TRUE, case.sensitive = TRUE, language = .language)
        res <- db.gpapply(mul, output.name = .output.name,
                        FUN = fn.function_4, output.signature = .signature,
                        clear.existing = TRUE, case.sensitive = TRUE, language = .language)
        res <- db.gpapply(mul, output.name = .output.name,
                        FUN = fn.function_5, output.signature = .signature,
                        clear.existing = TRUE, case.sensitive = TRUE, language = .language)
    }
    db.disconnect(cid)
}
