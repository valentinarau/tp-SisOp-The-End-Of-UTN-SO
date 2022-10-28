/*
 * consola.c
 *
 *  Created on: Sep 11, 2022
 *      Author: utnso
 */

#include <consola.h>

// ./consola.out ruta/del/config ruta/de/las/instrucciones  

int main(int argc, char **argv) {

	if(argc != 3){ 
		error_show("Argumentos invalidos!");
		return EXIT_FAILURE;
	}

	char* ruta_config = strdup(argv[1]); 
	char* ruta_instrucciones = strdup(argv[2]);
	t_list* instrucciones;

	pthread_t th_atender_solicitud_kernel;
	pthread_create(&th_atender_solicitud_kernel, NULL, &atender_solicitud_kernel, NULL);
	pthread_detach(th_atender_solicitud_kernel);

	/* LOGGER DE ENTREGA */
	//consola_logger = iniciar_logger(RUTA_LOGGER_CONSOLA, NOMBRE_MODULO, 1, LOG_LEVEL_INFO);
	
	/* LOGGER DE DEBUG */
	consola_logger = iniciar_logger(RUTA_LOGGER_DEBUG_CONSOLA, NOMBRE_MODULO, 1, LOG_LEVEL_DEBUG);
	log_debug(consola_logger,"Arrancando consola...");

	consola_config = cargar_configuracion(ruta_config, CONSOLA);
	log_debug(consola_logger,"Configuracion cargada correctamente");

	conexion_kernel = crear_conexion(consola_config->ip, consola_config->puerto);
	log_debug(consola_logger,"Conexion creada correctamente");

	char *instrucciones_string = leer_archivo_pseudocodigo(ruta_instrucciones);
	log_debug(consola_logger,"Archivo de pseudocodigo leido correctamente");

	instrucciones = obtener_pseudocodigo(instrucciones_string);
	log_debug(consola_logger,"Instrucciones parseadas correctamente");

	enviar_instrucciones(instrucciones, conexion_kernel);
	log_debug(consola_logger,"Instrucciones enviadas");

	liberar_conexion(conexion_kernel);

	log_debug(consola_logger, "termino consola"); 

	free(ruta_config);
	free(ruta_instrucciones);
	free(instrucciones_string);

	return EXIT_SUCCESS;
}

void* atender_solicitud_kernel(){
	while(1){
		cod_mensaje cod_msj = recibir_operacion(conexion_kernel);
		
		switch (cod_msj)
		{
			case FINALIZAR:
				/* code */
				break;
			case TECLADO:
				int valor_ingresado = ingresar_por_teclado();
				enviar_valor(valor_ingresado, conexion_kernel);
				break;
			case PANTALLA:
				imprimir_por_pantalla();
				
				break;
			default:
				
				error_show("Error, mensaje desconocido.\n");
				break;
		}
	}
}

void imprimir_por_pantalla(){
	int valor_a_imprimir = recibir_valor(conexion_kernel);
	printf("%d\n", valor_a_imprimir);
}

int ingresar_por_teclado(){
	int valor_ingresado;
	scanf("%d",valor_ingresado);
	return valor_ingresado;
}

