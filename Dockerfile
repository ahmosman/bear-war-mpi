FROM ubuntu:22.04

RUN apt update && \
    apt install -y nano wget openssh-client openssh-server openmpi-bin openmpi-common libopenmpi-dev sudo iputils-ping g++ && \
    useradd -ms /bin/bash mpiuser && \
    echo "mpiuser:mpiuser" | chpasswd && \
    adduser mpiuser sudo && \
    echo "mpiuser ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers && \
    mkdir /var/run/sshd && \
    ssh-keygen -A && \
    su - mpiuser -c "ssh-keygen -t rsa -N '' -f /home/mpiuser/.ssh/id_rsa" && \
    cat /home/mpiuser/.ssh/id_rsa.pub >> /home/mpiuser/.ssh/authorized_keys && \
    chmod 700 /home/mpiuser/.ssh && \
    chmod 600 /home/mpiuser/.ssh/authorized_keys && \
    chown -R mpiuser:mpiuser /home/mpiuser/.ssh

USER root
COPY data /home/mpiuser/data
RUN chown -R mpiuser:mpiuser /home/mpiuser/data

USER mpiuser
WORKDIR /home/mpiuser

CMD sudo service ssh start && tail -f /dev/null