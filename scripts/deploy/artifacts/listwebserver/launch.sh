#!/bin/bash

bash /app/gen_static_config.sh &
service nginx start &
# Relaunch the GUI server if it exits: nothing else starts it again
while true; do node ./static_server.js /app/gui 8080; sleep 1; done &
node ./dist/server.js config.yml --dev &

# Relaunch if it crashes
while true; do ../bin/stream_pre_processor amqp://rabbitmq:5672 ; done
