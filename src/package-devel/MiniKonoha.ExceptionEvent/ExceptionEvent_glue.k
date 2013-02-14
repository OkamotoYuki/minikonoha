import("Minikonoha.EventListener");

void RuntimeScriptException(Event e) {
	System.p("RuntimeScriptException");
	System.p("file:" + e.getProperty("file"));
	// you can write the way for out of bounds exception
}

void IndexOutOfBoundsException(Event e) {
	// you can write the way for out of bounds exception
}

// you can write other exception callbacks here

EventHandler.setCallBack("RuntimeScriptException", RuntimeScriptException);
EventHandler.setCallBack("IndexOutOfBoundsException", IndexOutOfBoundsException);
// you can list the callbacks here

HttpEventListener.start("127.0.0.1", 8080);
