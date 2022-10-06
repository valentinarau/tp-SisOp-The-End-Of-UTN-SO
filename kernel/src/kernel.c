/*
 * kernel.c
 *
 *  Created on: Sep 11, 2022
 *      Author: utnso
 */

#include <kernel.h>

// CONSOLA_FD PUEDE SER PPID

int main(void){
	t_kernel_config* kernel_config;
	
	/* LOGGER DE ENTREGA */
	//kernel_logger = iniciar_logger(RUTA_LOGGER_KERNEL, NOMBRE_MODULO, 1, LOG_LEVEL_INFO);
	
	/* LOGGER DE DEBUG */
	kernel_logger = iniciar_logger(RUTA_LOGGER_DEBUG_KERNEL, NOMBRE_MODULO, 1, LOG_LEVEL_DEBUG);

	log_debug(kernel_logger,"Arrancando kernel...");

	kernel_config = cargar_configuracion(RUTA_KERNEL_CONFIG, KERNEL);
	log_debug(kernel_logger,"Configuracion cargada correctamente");
	
	kernel_server_fd = iniciar_servidor(kernel_config->ip_kernel, kernel_config->puerto_escucha);

	conexion_cpu_dispatch = crear_conexion(kernel_config->ip_cpu, kernel_config->puerto_cpu_dispatch);
	if(conexion_cpu_dispatch != -1){
	log_debug(kernel_logger,"Conexion creada correctamente con CPU DISPATCH");
	}
	// TODO: conexion_cpu_interrupt = crear_conexion(kernel_config->ip_cpu, kernel_config->puerto_cpu_interrupt);

	planificacion_init(kernel_config);

	esperar_conexiones();

	liberar_conexion(conexion_cpu_dispatch);
	// TODO: liberar_conexion(conexion_cpu_interrupt);
	
	log_debug(kernel_logger,"Termino Kernel");

	return EXIT_SUCCESS;
}



u_int32_t siguiente_pid(){
	u_int32_t siguiente_pid = 0;
	pthread_mutex_lock(&pid_mutex);
	siguiente_pid = ++pid_actual;
	pthread_mutex_unlock(&pid_mutex);
	return siguiente_pid;
}

void* atender_cpu_dispatch(void* arg){
	while(1){
		// planificar
		sem_wait(&consolas);
		t_pcb *pcb = queue_pop(cola_ready_pcbs);
		log_debug(kernel_logger,"Estado PCB: %d",pcb->estado);
		// 
		enviar_pcb(pcb, conexion_cpu_dispatch);
		int cod_op = recibir_operacion(conexion_cpu_dispatch);
		switch (cod_op)
		{
		case PCB:
			pcb = recibir_pcb(conexion_cpu_dispatch);
			log_debug(kernel_logger,"PCB Recibida\n: %s",pcb_to_string(pcb));
			break;
		
		default:
			puts("error");
			break;
		}
		
		//
		
		dirigir_pcb(pcb);
		//pcb_destroy(pcb);
		//enviar_mensaje("dispatch", conexion_cpu_dispatch);

		// envias pcb elegido

		// recv (esperas pcb)

	}
}

void* rajar_pcb(void* arg) {
	while(1) {
		sem_wait(&procesos_finalizados);
		t_pcb* pcb = queue_pop(cola_exit_pcbs);
		log_debug(kernel_logger,"PCB con id: %d ha finalizado.",pcb->pid);
		sem_post(&multiprogramacion);
	}
}

void planificacion_init(t_kernel_config* kernel_config) {
	
	cola_new_pcbs = queue_create();
	cola_exit_pcbs = queue_create();
	cola_ready_pcbs = queue_create();

	//colas_init(kernel_config);
	sem_init(&conexiones,0,0);
	sem_init(&consolas,0,0);
	sem_init(&multiprogramacion,0,kernel_config->grado_multiprogramacion);
	sem_init(&procesos_finalizados, 0, 0);
	pthread_mutex_init(&pid_mutex, NULL);
	pthread_mutex_init(&cola_new_pcbs_mutex, NULL);
	pid_actual = 0;

	pthread_t thread_cpu_dispatch;
	pthread_t thread_consola;
	pthread_t thread_rajar_pcb;
	pthread_create(&thread_cpu_dispatch, NULL, &atender_cpu_dispatch, NULL);
	pthread_create(&thread_rajar_pcb, NULL, &rajar_pcb, NULL);
	pthread_create(&thread_consola, NULL, &atender_consolas, NULL);
	pthread_detach(thread_consola);
	pthread_detach(thread_cpu_dispatch);
	pthread_detach(thread_rajar_pcb);
}

void iterator(instruccion* value) {
	log_debug(kernel_logger,"%d", value->operacion);
	log_debug(kernel_logger,"%s", value->parametro1);
	log_debug(kernel_logger,"%s", value->parametro2);
}

void dirigir_pcb(t_pcb* pcb){

	int ultima_instruccion_idx = pcb->program_counter - 1;
	puts(string_itoa(ultima_instruccion_idx));

	instruccion* ultima_instruccion = list_get(pcb->instrucciones,ultima_instruccion_idx);
	switch(ultima_instruccion->operacion){
		case EXIT:
			pcb->estado = FINISH_EXIT;
			queue_push(cola_exit_pcbs,pcb);
			log_info(kernel_logger,"PID: <PID> - Estado Anterior: <ESTADO_ANTERIOR> - Estado Actual: <ESTADO_ACTUAL>");
			sem_post(&procesos_finalizados);
			break;
		case IO:
			pcb->estado = BLOCK;
			// manejar IO
			break;
		default:
			if(pcb->interrupcion){
				pcb->estado = BLOCK;
				// manejar quantum
				log_info(kernel_logger,"PID: <PID> - Estado Anterior: <ESTADO_ANTERIOR> - Estado Actual: <ESTADO_ACTUAL>");
			} else {
				pcb->estado = READY;
				queue_push(cola_ready_pcbs,pcb);
				log_info(kernel_logger,"PID: <PID> - Estado Anterior: <ESTADO_ANTERIOR> - Estado Actual: <ESTADO_ACTUAL>");
			}
			break;
	}
}


void esperar_conexiones(){
	cola_consolas = queue_create();

	while(1){
		log_debug(kernel_logger,"Soy Kernel. Esperando conexion...");
		int consola_fd = esperar_cliente(kernel_server_fd);
		log_debug(kernel_logger, "se conecto un cliente");
		// mutex
		queue_push(cola_consolas, (void*) (intptr_t) consola_fd);
		// mutex
		sem_post(&conexiones);
	}
}

void* atender_consolas(void* arg){ 
	t_pcb* pcb;
	t_list* instrucciones;
	while(1){
		sem_wait(&conexiones);
		// mutex
		int consola_fd = (int) (intptr_t) queue_pop(cola_consolas);
		//mutex
		int cod_op = recibir_operacion(consola_fd);
		switch (cod_op) {
			case MENSAJE:
				recibir_mensaje(kernel_logger, consola_fd);
				break;
			case PAQUETE_INSTRUCCIONES:
				instrucciones = recibir_paquete_con_funcion(consola_fd, deserializar_instruccion);
				log_debug(kernel_logger, "Recibí %d instrucciones", list_size(instrucciones));
				list_iterate(instrucciones, (void*) iterator);
				pcb = pcb_create(instrucciones, siguiente_pid(), consola_fd);
				pthread_mutex_lock(&cola_new_pcbs_mutex);
				// Agregar pcb a cola new
				queue_push(cola_new_pcbs,pcb);
				pthread_mutex_unlock(&cola_new_pcbs_mutex);
				log_info(kernel_logger,"Se crea el proceso %d en NEW", pcb->pid);
				// Si el grado de multiprogramacion lo permite, lo pasa a ready
				sem_wait(&multiprogramacion);
				// mutex
				pcb = queue_pop(cola_new_pcbs);
				// mutex
				pcb->estado = READY;
				// mutex
				queue_push(cola_ready_pcbs, pcb);
				// mutex
				sem_post(&consolas);
				break;
			case -1:
				log_debug(kernel_logger, "Conexion con consola finalizada");
				return NULL;
			default:
				error_show("Operacion desconocida");
				break;
		}

		
	}
}

// void colas_init(t_kernel_config* kernel_config){
	
// 	//cola_ready_FIFO_pcbs = queue_create();
// 	//cola_ready_RR_pcbs = queue_create();
// }


// Preguntas checkpint

// De quien es responsabilidad cambiar el estado de un pcb? Dentro de que funcion?
// Como se supone que vamos a abstraer y separar la plani de largo y la plani de corto?
// Por que a veces los modulos se conectan bien y otras veces mal.
// Hay que detachear los demas threads? cpu dispatch, cpu interrupt, rajar_pcb?
// Como se espera (en que orden) que se levanten los modulos? Para la entrega final.
// mediano plazo


// void* atender_consola(void* p){
// 	int consola_fd = *(int *) p;
// 	free(p);
// 	t_pcb* pcb;
// 	t_list* instrucciones;
// 		while(1){
		
// 	}
// }
