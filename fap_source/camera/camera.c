#include "camera.h"

static void camera_view_draw_callback(Canvas* canvas, void* _model) {
    UartDumpModel* model = _model;

    // Prepare canvas
    //canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 0, 0, FRAME_WIDTH, FRAME_HEIGTH); 
    
    for(size_t p = 0; p < FRAME_BUFFER_LENGTH; ++p) {
        uint8_t x = row_lookup[p];
        uint8_t y = col_lookup[p];
        uint8_t fb_value = model->fb[p];

        for (uint8_t i = 0; i < 4; ++i) {
            uint8_t color = colors_lookup[fb_value][i];
            
            if (color == 0 ||
                ((color == 85) && (model->frame_count != 0)) ||
                ((color == 170) && (model->frame_count == 0))) {
                canvas_draw_dot(canvas, (x * 4) + i, y);
            }
        }
    }

    if (!model->initialized){
        canvas_draw_icon(canvas, 74, 16, &I_DolphinCommon_56x48);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 8, 12, "Connect the ESP32-CAM");
        canvas_draw_str(canvas, 20, 24, "VCC - 3V3");
        canvas_draw_str(canvas, 20, 34, "GND - GND");
        canvas_draw_str(canvas, 20, 44, "U0R - TX");
        canvas_draw_str(canvas, 20, 54, "U0T - RX");
    }

    if (++model->frame_count == 3){
        model->frame_count = 0;
    }

    for (size_t i = 0; i < 1024; i++)
    {
        canvas->fb.tile_buf_ptr[i] = 170;
    }
    //canvas_commit(canvas);
    
}

void get_timefilename(FuriString* name) {
    FuriHalRtcDateTime datetime = {0};
    furi_hal_rtc_get_datetime(&datetime);
    furi_string_printf(
        name,
        EXT_PATH("DCIM/%.4d%.2d%.2d-%.2d%.2d%.2d.bmp"),
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second);
}

static void save_image(void* context) {
    CameraApp* app = context;
    furi_assert(app);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    // We need a storage struct (gain accesso to the filesystem API )
    Storage* storage = furi_record_open(RECORD_STORAGE);

    // storage_file_alloc gives to us a File pointer using the Storage API.
    File* file = storage_file_alloc(storage);

    if(storage_common_stat(storage, IMAGE_FILE_DIRECTORY_PATH, NULL) == FSE_NOT_EXIST) {
        storage_simply_mkdir(storage, IMAGE_FILE_DIRECTORY_PATH);
    }

    // create file name
    FuriString* file_name = furi_string_alloc();
    get_timefilename(file_name);

    // this functions open a file, using write access and creates new file if not exist.
    bool result = storage_file_open(file, furi_string_get_cstr(file_name), FSAM_WRITE, FSOM_OPEN_ALWAYS);
    //bool result = storage_file_open(file, EXT_PATH("DCIM/test.bmp"), FSAM_WRITE, FSOM_OPEN_ALWAYS);
    furi_string_free(file_name);

    if (result){
        storage_file_write(file, bitmap_header, BITMAP_HEADER_LENGTH);
        with_view_model(app->view, UartDumpModel * model, {
            int8_t row_buffer[ROW_BUFFER_LENGTH];
            for (size_t i = 64; i > 0; --i) {
                for (size_t j = 0; j < ROW_BUFFER_LENGTH; ++j){
                    row_buffer[j] = model->fb[((i-1)*ROW_BUFFER_LENGTH) + j];
                }
                storage_file_write(file, row_buffer, ROW_BUFFER_LENGTH);
            }
            
        }, false);
    }

    // Closing the "file descriptor"
    storage_file_close(file);

    // Freeing up memory
    storage_file_free(file);

    notification_message(notifications, result ? &sequence_success : &sequence_error);
}

static void uart_send(char c){
    uint8_t data[1];
    data[0] = c;
    furi_hal_uart_tx(FuriHalUartIdUSART1, data, 1);
}

static bool camera_view_input_callback(InputEvent* event, void* context) {
    //furi_assert(context);
    //CameraApp* app = context;

    if (event->type == InputTypePress){
        if (event->key == InputKeyUp){
            uart_send('+');
        }
        else if (event->key == InputKeyDown){
            uart_send('M');
        }
        else if (event->key == InputKeyRight){
            uart_send('>');
        }
        else if (event->key == InputKeyLeft){
            uart_send('<');
        }//save image does not work with 2 bit depth yet.
        else if (event->key == InputKeyOk){
            save_image(context);
        }
    }
    
    return false;
}

static uint32_t camera_exit(void* context) {
    uart_send('s');
    UNUSED(context);
    return VIEW_NONE;
}

static void camera_on_irq_cb(UartIrqEvent ev, uint8_t data, void* context) {
    furi_assert(context);
    CameraApp* app = context;

    if(ev == UartIrqEventRXNE) {
        furi_stream_buffer_send(app->rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventRx);
    }
}

static void process_ringbuffer(UartDumpModel* model, uint8_t byte) {
    //// 1. Phase: filling the ringbuffer
    if (model->ringbuffer_index == 0 && byte != 'Y'){ // First char has to be 'Y' in the buffer.
        return;
    }
    
    if (model->ringbuffer_index == 1 && byte != '2'){ // Second char has to be ':' in the buffer or reset.
        model->ringbuffer_index = 0;
        process_ringbuffer(model, byte);
        return;
    }

    model->row_ringbuffer[model->ringbuffer_index] = byte; // Assign current byte to the ringbuffer;
    ++model->ringbuffer_index; // Increment the ringbuffer index

    if (model->ringbuffer_index < RING_BUFFER_LENGTH){ // Let's wait 'till the buffer fills.
        return;
    }

    //// 2. Phase: flushing the ringbuffer to the framebuffer
    model->ringbuffer_index = 0; // Let's reset the ringbuffer
    model->initialized = true; // We've successfully established the connection
    size_t row_start_index = model->row_ringbuffer[2] * ROW_BUFFER_LENGTH; // Third char will determine the row number

    if (row_start_index > LAST_ROW_INDEX){ // Failsafe
        row_start_index = 0;
    }

    for (size_t i = 0; i < ROW_BUFFER_LENGTH; ++i) {
        model->fb[row_start_index + i] = model->row_ringbuffer[i+3]; // Writing the remaining 16 bytes into the frame buffer
    }
}

static int32_t camera_worker(void* context) {
    furi_assert(context);
    CameraApp* app = context;

    vTaskPrioritySet(furi_thread_get_current_id(), FuriThreadPriorityIdle);

    while(1) {
        uint32_t events = furi_thread_flags_wait(WORKER_EVENTS_MASK, FuriFlagWaitAny, 0);
        //furi_check((events & FuriFlagError) == 0);

        if(events & WorkerEventStop) break;
        if(events & WorkerEventRx) {
            size_t length = 0;
            do {
                size_t intended_data_size = 64;
                uint8_t data[intended_data_size];
                length = furi_stream_buffer_receive(app->rx_stream, data, intended_data_size, 0);

                if(length > 0) {
                    with_view_model(
                        app->view,
                        UartDumpModel * model, {
                            for(size_t i = 0; i < length; i++) {
                                process_ringbuffer(model, data[i]);
                            }
                        },
                        false);
                }
            } while(length > 0);

            //with_view_model(app->view, UartDumpModel * model, { UNUSED(model); }, true);
        }
        with_view_model(app->view, UartDumpModel * model, { UNUSED(model); }, true);
    }

    return 0;
}

static void precompute_lookup_tables() {
    for(size_t i = 0; i < FRAME_BUFFER_LENGTH; i++) {
        row_lookup[i] = i % ROW_BUFFER_LENGTH; // 0 .. 15
        col_lookup[i] = i / ROW_BUFFER_LENGTH; // 0 .. 63
    }

    for (uint8_t byte = 0; ; ++byte){
        for (uint8_t i = 0; i < 4; ++i){ // 4 grayscale pixel in 1 fb element
            bool first_bit = byte & (1 << (7 - (i*2)));
            bool second_bit = byte & (1 << (7 - ((i*2)+1)));
            // 0 = 00, 85 = 01, 170 = 10, 255 = 11
            if(!second_bit && !first_bit){  // 00
                colors_lookup[byte][i] = 0;
            }
            else if ((!second_bit && first_bit)){   // 01
                colors_lookup[byte][i] = 85;
            }
            else if ((second_bit && !first_bit)) {  // 10
                colors_lookup[byte][i] = 170;
            }
            else {  // 11
                colors_lookup[byte][i] = 255;
            }
        }

        if (byte == UINT8_MAX){
            break;
        }
    }
}

static CameraApp* camera_app_alloc() {
    CameraApp* app = malloc(sizeof(CameraApp));

    app->rx_stream = furi_stream_buffer_alloc(2048, 1);

    // Gui
    app->gui = furi_record_open(RECORD_GUI);
    app->notification = furi_record_open(RECORD_NOTIFICATION);

    // View dispatcher
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Views
    app->view = view_alloc();
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, camera_view_draw_callback);
    view_set_input_callback(app->view, camera_view_input_callback);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(UartDumpModel));

    with_view_model(
        app->view,
        UartDumpModel * model,
        {
            for(size_t i = 0; i < FRAME_BUFFER_LENGTH; i++) {
                model->fb[i] = 255;
            }
        },
        true);

    view_set_previous_callback(app->view, camera_exit);
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    app->worker_thread = furi_thread_alloc_ex("UsbUartWorker", 2048, camera_worker, app);
    furi_thread_start(app->worker_thread);

    // Enable uart listener
    furi_hal_console_disable();
    furi_hal_uart_set_br(FuriHalUartIdUSART1, 230400);
    furi_hal_uart_set_irq_cb(FuriHalUartIdUSART1, camera_on_irq_cb, app);
    
    uart_send('S');
    uart_send('2');

    notification_message_block(app->notification, &sequence_display_backlight_enforce_on);

    precompute_lookup_tables();

    return app;
}

static void camera_app_free(CameraApp* app) {
    furi_assert(app);

    furi_hal_console_enable(); // this will also clear IRQ callback so thread is no longer referenced

    furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventStop);
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    // Free views
    view_dispatcher_remove_view(app->view_dispatcher, 0);

    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    notification_message(app->notification, &sequence_display_backlight_enforce_auto);

    // Close gui record
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    app->gui = NULL;

    furi_stream_buffer_free(app->rx_stream);

    // Free rest
    free(app);
}

int32_t camera_app(void* p) {
    UNUSED(p);
    CameraApp* app = camera_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    camera_app_free(app);
    return 0;
}
