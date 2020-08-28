import cups

def do_indent (indent):
	return "  "*indent

def getippqueue (dev, queue, depth):
	name = dev.rfind ('/')
	name = dev[name + 1:]
	dev = dev[6:]
	e = dev.find (':')
	if e == -1:
		e = dev.find ('/')
	host = dev[:e]
	cups.setServer (host)
	try:
		c = cups.Connection ()
		printers = c.getPrinters ()
		classes = c.getClasses ()
	except RuntimeError:
		# Failed to connect.
		return
	except cups.IPPError as e:
		if e == cups.IPP_OPERATION_NOT_SUPPORTED:
			# CUPS-Get-Printers not supported so not a CUPS server.
			printers = {}
			classes = {}
		else:
			return

	queue = c.getPrinterAttributes (name)
	dev = queue['device-uri']
	getqueue (name, queue, host, depth + 1, printers, classes)

def getqueue (name, queue, host, depth, printers, classes):
	indent = do_indent(depth)
	if queue['printer-type'] & cups.CUPS_PRINTER_CLASS:
		print ("%s* Name:\t%s[@%s] (class)" % (indent, name, host))
		dev = queue['device-uri']
		if dev.startswith ('ipp:'):
			getippqueue (dev, queue, depth)
		else:
			members = classes[name]
			depth += 1
			indent = do_indent(depth)
			for member in members:
				getqueue (member, printers[member], host,
					  depth, printers, classes)
	else:
		print ("%s* Name:\t%s[@%s]" % (indent, name, host))
		dev = queue['device-uri']
		info = queue['printer-info']
		print ("%sURI:\t%s" % (indent, dev))
		print ("%sInfo:\t%s" % (indent, info))
		if dev.startswith ('ipp:'):
			getippqueue (dev, name, depth)

	if depth == 0:
		print ("")

def gethost (host=None, depth=0):
	if host:
		cups.setServer (host)
	else:
		host = "localhost"
	c = cups.Connection ()
	printers = c.getPrinters ()
	classes = c.getClasses ()
	indent = do_indent(depth)
	for name, queue in printers.items ():
		getqueue (name, queue, host, depth, printers, classes)

gethost()
