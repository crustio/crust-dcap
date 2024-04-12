# crust sworker image
FROM crustio/dcap-service-env:1.0.0

# add and build sr25519 source code
ADD sr25519 /dcap-service/sr25519
RUN cd /dcap-service/sr25519 && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make install

# add and build dcap-service source code
ADD scripts /dcap-service/scripts
ADD src /dcap-service/src
ADD VERSION /dcap-service/VERSION
ADD Makefile /dcap-service/Makefile
ADD Config.json /dcap-service/Config.json
RUN /dcap-service/scripts/install.sh -d

# add start script
ADD docker/runner/start_dcap_service.sh /
CMD /start_dcap_service.sh $ARGS
