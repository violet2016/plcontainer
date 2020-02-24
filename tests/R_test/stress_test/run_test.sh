#!/bin/bash
# The number of connection x 5
NUM_CONN=$(($1))

# The number of iteration  
NUM_ITER=$(($2))
test_concurrent () { 
  for ((i=1; i<=$NUM_CONN; i++)); do
    echo "Test $i has started"
    ./run_test.R $NUM_ITER >> log/test_${j}_${i}.log & pid=$!
    PSQL_LIST+=" $pid"
  done
  trap "kill -9 $PSQL_LIST" SIGINT
  wait $PSQL_LIST
}
rm -rf log
mkdir -p log
time test_concurrent