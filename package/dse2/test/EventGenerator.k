import("konoha.while");
import("posix.process");
import("dse2");

void enqEvent(Event e) {
	System.p(e.getProperty("event"));
}

void test() {
	System.setEnqFunc(enqEvent);
	HttpEventGenerator.start("0.0.0.0", 8080);
	while(true) {
		System.sleep(1);
	}
}

test();
