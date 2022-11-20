#include <kernel_utils.h>

int conexion_memoria;
int conexion_cpu_dispatch;
int conexion_cpu_interrupt;
int kernel_server_fd;
int cantidad_dispositivos;

t_log* kernel_logger;
t_kernel_config* kernel_config;
t_list* dispositivos_io;


t_queue* cola_new_pcbs;
t_queue* cola_exit_pcbs;
t_queue* cola_ready_FIFO_pcbs;
t_queue* cola_ready_RR_pcbs;

sem_t procesos_ready;
sem_t procesos_new;
sem_t multiprogramacion;
sem_t proceso_page_fault;
sem_t procesos_finalizados; 
sem_t interrupcion_quantum;
sem_t* s_dispositivos_io;
// chequear como inicializar array de semaforos

int pid_actual;
t_algoritmo algoritmo;

pthread_mutex_t pid_mutex;
pthread_mutex_t cola_new_pcbs_mutex;
pthread_mutex_t cola_ready_RR_pcbs_mutex;
pthread_mutex_t cola_ready_FIFO_pcbs_mutex;
pthread_mutex_t cola_exit_pcbs_mutex;
pthread_mutex_t* cola_dispositivo_mutex;

pthread_t th_timer;
pthread_t th_conexiones;
pthread_t th_ejecucion;
pthread_t th_transiciones_ready;
pthread_t th_manejo_page_fault;
pthread_t th_rajar_pcb;

/* Planificacion */

/* Largo Plazo */

void largo_plazo_init(){
    cola_new_pcbs = queue_create();
	cola_exit_pcbs = queue_create();
    
	sem_init(&procesos_new,0,0);
	sem_init(&procesos_finalizados, 0, 0);
	pthread_mutex_init(&cola_new_pcbs_mutex, NULL);
	pthread_mutex_init(&cola_exit_pcbs_mutex, NULL);

    pthread_create(&th_rajar_pcb, NULL, &rajar_pcb, NULL);
	pthread_create(&th_conexiones, NULL, &atender_nueva_consola, NULL);
	pthread_detach(th_rajar_pcb);
}

void* atender_nueva_consola(void* arg){
    t_pcb* pcb;
	t_list* instrucciones;
	t_list* segmentos;
	t_proceso* proceso;
	while(1){
		log_debug(kernel_logger, "Soy Kernel. Esperando conexion...");
		int consola_fd = esperar_cliente(kernel_server_fd);
		log_debug(kernel_logger, "se conecto un cliente");
        cod_mensaje cod_msj = recibir_operacion(consola_fd);
        if(cod_msj == PROCESO){
            proceso = deserializar_proceso(consola_fd);
			instrucciones = proceso->instrucciones;
			segmentos = proceso->segmentos;
            log_debug(kernel_logger, "Recibí %d instrucciones", list_size(instrucciones));
            log_debug(kernel_logger, "Recibí %d segmentos", list_size(segmentos));
            pcb = pcb_create(proceso, siguiente_pid(), consola_fd);
            safe_pcb_push(cola_new_pcbs, pcb, &cola_new_pcbs_mutex);
            log_info(kernel_logger, "Se crea el proceso %d en NEW", pcb->pid);
			free(proceso);
			sem_post(&procesos_new);
        } else {
			error_show("Mensaje desconocido");
		}
	}
}

void* rajar_pcb(void* arg) {
	while(1) {
		sem_wait(&procesos_finalizados);
		t_pcb* pcb = safe_pcb_pop(cola_exit_pcbs, &cola_exit_pcbs_mutex);
		log_debug(kernel_logger,"PCB con id: %d ha finalizado.",pcb->pid);
		pcb_destroy(pcb);
		sem_post(&multiprogramacion);
	}
}

/* Corto Plazo */

void corto_plazo_init(){
    cola_ready_FIFO_pcbs = queue_create();
	cola_ready_RR_pcbs = queue_create();

    sem_init(&procesos_ready,0,0);
    sem_init(&proceso_page_fault,0,0);
    pthread_mutex_init(&cola_ready_RR_pcbs_mutex, NULL);
	pthread_mutex_init(&cola_ready_FIFO_pcbs_mutex, NULL);

    pthread_create(&th_ejecucion, NULL, &planificar_ejecucion, NULL);
	pthread_create(&th_transiciones_ready, NULL, &transicion_proceso_a_ready, NULL);

    pthread_detach(th_transiciones_ready);
	pthread_detach(th_ejecucion);
}

void* planificar_ejecucion(void* arg){ 
	while(1){
		t_pcb *pcb = seleccionar_pcb();
        planificar_interrupcion(pcb);
        enviar_pcb(pcb, conexion_cpu_dispatch);
        pcb_destroy(pcb);
        pcb = obtener_proceso_ejecutado();
        analizar_contexto_recibido(pcb);
	    dirigir_proceso_ejecutado(pcb);
	}
}

t_pcb* seleccionar_pcb(){
    sem_wait(&procesos_ready);
    t_pcb *pcb = pop_ready_pcb();
    log_debug(kernel_logger,"Estado PCB: %d",pcb->estado);
    cambiar_estado(pcb, EXEC);
    log_debug(kernel_logger, "Enviando PCB");
    return pcb;
}

void planificar_interrupcion(t_pcb* pcb){
    if(pcb->con_desalojo){
        log_debug(kernel_logger, "Preparando interrupcion");
        iniciar_interrupcion();
        sem_post(&interrupcion_quantum);
    }
}

t_pcb* obtener_proceso_ejecutado(){
    t_pcb *pcb;
    int cod_op = recibir_operacion(conexion_cpu_dispatch);
    if(cod_op == PCB) {
        pcb = recibir_pcb(conexion_cpu_dispatch);
        return pcb;
    }
    else {
        error_show("Error al recibir PCB");
		pthread_exit(NULL);
    }  
}
void* manejar_page_fault(void* arg)
{	
	while(1){
		sem_wait(&proceso_page_fault);
		t_pcb* pcb = (t_pcb*) arg;
		log_debug(kernel_logger, "Enviando pagina fault: %d", pcb->pagina_fault->numero_pagina);
		enviar_pagina(pcb->pagina_fault, conexion_memoria);
		cod_mensaje cod_mensaje_memoria = recibir_operacion(conexion_memoria); //consultar si es correcto
		if(cod_mensaje_memoria == OKI_PAGINA)
		{
			log_debug(kernel_logger, "page fault solucionado pid: %d", pcb->pid);
			pasar_a_ready(pcb);
			pthread_exit(NULL);
		}
		else {
			error_show("Error en page fault");
			pthread_exit(NULL);
		}
	}
	 
}


void analizar_contexto_recibido(t_pcb* pcb){
    if(pcb->con_desalojo) {
		pthread_cancel(th_timer);
		log_debug(kernel_logger, "Interrupcion cancelada por vuelta del PCB");
	}
	if(pcb->interrupcion){ // cuando hay fin de quantum
		log_info(kernel_logger,"PID: %d - Desalojado por fin de Quantum", pcb->pid);
		pcb->interrupcion = false;
		pcb->con_desalojo = false;
	}

}


void dirigir_proceso_ejecutado(t_pcb* pcb){ // corto plazo // tener en cuenta page default ya que no deberiamos modificar el program counter
	instruccion* ultima_instruccion = obtener_ultima_instruccion(pcb);
	if(pcb->segmentation_fault) solicitar_finalizacion(pcb);
	if(pcb->page_fault) {
		cambiar_estado(pcb, BLOCK);
		log_debug(kernel_logger, "page faulted PCB pid: %d", pcb->pid);
		pthread_create(&th_manejo_page_fault, NULL, &manejar_page_fault, (void*) pcb);
		pthread_detach(th_manejo_page_fault);
		sem_post(&proceso_page_fault);
	}
	else {
		switch(ultima_instruccion->operacion){
			case EXIT:
				solicitar_finalizacion(pcb);
				break;
			case IO:
				solicitar_io(pcb, ultima_instruccion);
				break;
			default:
				pasar_a_ready(pcb);
				break;
		}
	}
}

instruccion* obtener_ultima_instruccion(t_pcb* pcb){
	int ultima_instruccion_idx = pcb->program_counter - 1;
	instruccion* ultima_instruccion = list_get(pcb->instrucciones,ultima_instruccion_idx);
	return ultima_instruccion;
}

void* transicion_proceso_a_ready(void* arg){
	while(1){
		sem_wait(&procesos_new);
        sem_wait(&multiprogramacion);
        t_pcb* pcb = safe_pcb_pop(cola_new_pcbs, &cola_new_pcbs_mutex);
		solicitar_creacion_estructuras_administrativas(pcb);
        pasar_a_ready(pcb);
	}
}
void solicitar_creacion_estructuras_administrativas(t_pcb* pcb) {
	t_pcb_memoria* pcb_memoria = malloc(sizeof(t_pcb_memoria));
	pcb_memoria->pid = pcb->pid;
	pcb_memoria->segmentos = pcb->tamanio_segmentos;
	enviar_pcb_memoria(pcb_memoria, conexion_memoria);
 	cod_mensaje mensaje = recibir_operacion(conexion_memoria);

 	if(mensaje == OKI_ESTRUCTURAS){
		t_list* indices = recibir_indices_tabla_paginas(conexion_memoria);
		pcb->tabla_de_segmentos = crear_tabla_segmentos(indices, pcb->tamanio_segmentos); // sacar cuando no se use
		log_debug(kernel_logger, "Se han creado las estructuras para el proceso %d, %d tablas", pcb->pid, list_size(pcb->tabla_de_segmentos));
		list_destroy_and_destroy_elements(indices, free);
		free(pcb_memoria);
	}
	else {
		error_show("Error al crear estructuras");
	}
 }

t_list* crear_tabla_segmentos(t_list* indices, t_list* tamanio_segmentos){
	t_list* tabla = list_create();
	for(int i = 0; i < list_size(indices); i++){
		char* tamanio = list_get(tamanio_segmentos, i);
		char* indice = list_get(indices, i);
		t_segmento* segmento = segmento_create(i, atoi(indice), atoi(tamanio));
		list_add(tabla, segmento);
	}
	return tabla;
}

/* Planificacion Utils */

void cambiar_estado(t_pcb* pcb, estado_proceso nuevo_estado){
	char* nuevo_estado_string = strdup(estado_to_string(nuevo_estado));
	char* estado_anterior_string = strdup(estado_to_string(pcb->estado));
	log_info(kernel_logger,"PID: %d - Estado Anterior: %s - Estado Actual: %s", pcb->pid, estado_anterior_string, nuevo_estado_string);
	pcb->estado = nuevo_estado;
	free(estado_anterior_string);
	free(nuevo_estado_string);
}

void pasar_a_ready(t_pcb* pcb){
    push_ready_pcb(pcb);
    cambiar_estado(pcb, READY);
    sem_post(&procesos_ready);
}

void solicitar_finalizacion(t_pcb* pcb){
	cod_mensaje cod_msj_consola = FINALIZAR;
	cod_mensaje cod_msj_memoria = LIBERAR_ESTRUCTURAS;
    cambiar_estado(pcb, FINISH_EXIT);
	safe_pcb_push(cola_exit_pcbs, pcb, &cola_exit_pcbs_mutex);
	enviar_valor_con_codigo(pcb->pid, cod_msj_memoria, conexion_memoria);
	cod_msj_memoria = recibir_operacion(conexion_memoria);
	if(cod_msj_memoria == OKI_LIBERAR_ESTRUCTURAS){
		enviar_datos(pcb->socket_consola,&cod_msj_consola,sizeof(cod_msj_consola));
	}
    sem_post(&procesos_finalizados);
}

void iniciar_interrupcion() {
 	pthread_create(&th_timer, NULL, &enviar_interrupt, NULL);
 	pthread_detach(th_timer);
 }

void* enviar_interrupt(void* arg){
	while(1) {
		sem_wait(&interrupcion_quantum);
		cod_mensaje mensaje = INTERRUPCION;
		log_debug(kernel_logger,"Hola! Soy hilo cpu interrupt");
		ejecutar_espera(kernel_config->quantum_RR);
		log_debug(kernel_logger,"Finalizo Espera");
		enviar_datos(conexion_cpu_interrupt,&mensaje, sizeof(mensaje));
		log_debug(kernel_logger, "Se envia mensaje de interrupcion a cpu \n");
	}
}

u_int32_t siguiente_pid(){
	u_int32_t siguiente_pid = 0;
	pthread_mutex_lock(&pid_mutex);
	siguiente_pid = ++pid_actual;
	pthread_mutex_unlock(&pid_mutex);
	return siguiente_pid;
}

t_pcb* pop_ready_pcb(){
	t_pcb* pcb;
	pthread_mutex_lock(&cola_ready_RR_pcbs_mutex);
	bool empty = queue_is_empty(cola_ready_RR_pcbs);
	pthread_mutex_unlock(&cola_ready_RR_pcbs_mutex);
	if((algoritmo == FEEDBACK && !empty) || algoritmo == RR){
        pcb = safe_pcb_pop(cola_ready_RR_pcbs, &cola_ready_RR_pcbs_mutex);
	} else {
		pcb = safe_pcb_pop(cola_ready_FIFO_pcbs, &cola_ready_FIFO_pcbs_mutex);
	}
	return pcb;
}

void push_ready_pcb(t_pcb* pcb){
	if((algoritmo == FEEDBACK && (pcb->estado == NEW || pcb->estado == BLOCK)) || algoritmo == RR) {
		pcb->con_desalojo = true;
		// saque un pcb->interrupcion = true; TODO: VALIDAR con las pruebas si esta bien
        safe_pcb_push(cola_ready_RR_pcbs, pcb, &cola_ready_RR_pcbs_mutex);
	} else {
		pcb->con_desalojo = false;
        safe_pcb_push(cola_ready_FIFO_pcbs, pcb, &cola_ready_FIFO_pcbs_mutex);
	}
		pcb->interrupcion = false;
		pcb->page_fault = false;
		pcb->segmentation_fault = false;
}

/* Utils */

void safe_pcb_push(t_queue* queue, t_pcb* pcb, pthread_mutex_t* mutex){
    pthread_mutex_lock(mutex);
    queue_push(queue, pcb);
    pthread_mutex_unlock(mutex);
}

t_pcb* safe_pcb_pop(t_queue* queue, pthread_mutex_t* mutex){
    t_pcb* pcb;
    pthread_mutex_lock(mutex);
    pcb = queue_pop(queue);
    pthread_mutex_unlock(mutex);
    return pcb;
}

