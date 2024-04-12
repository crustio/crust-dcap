# crust dcap-service env image
FROM ubuntu:18.04

# install build depends
RUN apt-get update
RUN apt-get install -y wget expect unzip jq cracklib-runtime

# install dcap dependencies
ADD scripts /dcap-service-env/scripts
ADD Config.json /dcap-service-env/Config.json
RUN /dcap-service-env/scripts/install_deps.sh 1

# install sr25519 dependencies
ADD sr25519/scripts /dcap-service-env/sr25519/scripts
RUN /dcap-service-env/sr25519/scripts/install_dependencies.sh